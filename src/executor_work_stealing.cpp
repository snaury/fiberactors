#include <fiberactors/executor_work_stealing.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <random>
#include <deque>
#include <cassert>

namespace fiberactors {

    class TWorkStealingExecutor : public IExecutor {
    private:
        static constexpr size_t MaxLocalTasks = 256;
        static constexpr uint32_t TaskIndexMask = 255;

        using TClock = std::chrono::steady_clock;
        using TTime = std::chrono::time_point<TClock>;

        struct TThreadState {
            TWorkStealingExecutor* Executor;
            size_t ThreadIndex;

            // The only reason these are atomic is to make it possible to
            // atomically read them while racing with other threads. All
            // operations on these atomics are relaxed.
            std::atomic<IRunnable*> LocalQueue[MaxLocalTasks];

            // Packed head and tail indexes to simplify consistent load and cas
            std::atomic<uint64_t> LocalQueueHeadTail{ 0 };

            // The number of processed local tasks, used to periodically check
            // the global queue.
            size_t LocalProcessed = 0;

            // Random state for stealing
            std::mt19937 Random;

            // The current preemption deadline
            TTime PreemptDeadline{};

            TThreadState(TWorkStealingExecutor* executor, size_t threadIndex)
                : Executor(executor)
                , ThreadIndex(threadIndex)
            {}
        };

    public:
        TWorkStealingExecutor(const TWorkStealingExecutorSettings& settings)
            : Settings(settings)
        {
            for (size_t i = 0; i < Settings.MaxThreads; ++i) {
                ThreadStates.emplace_back(this, i);
            }
            for (size_t i = 0; i < Settings.MaxThreads; ++i) {
                Threads.emplace_back([this, state = &ThreadStates[i]] {
                    this->RunWorker(state);
                });
            }
        }

        ~TWorkStealingExecutor() {
            std::unique_lock g(Mutex);
            BlockedThreads.fetch_or(FlagShutdown, std::memory_order_seq_cst);
            WakeUp.notify_all();
            g.unlock();

            for (auto& thread : Threads) {
                thread.join();
            }
        }

        void Post(IRunnable* r) override {
            auto* state = LocalState;
            if (state && state->Executor == this) [[likely]] {
                while (!PushLocalTask(state, r)) {
                    OffloadLocalTasks(state);
                }
                WakeByLocalTasks();
            } else {
                PushGlobalTask(r);
            }
        }

        void Defer(IRunnable* r) override {
            auto* state = LocalState;
            if (state && state->Executor == this) [[likely]] {
                while (!PushLocalTask(state, r)) {
                    OffloadLocalTasks(state);
                }
                // Note: we don't call WakeByLocalTasks
            } else {
                PushGlobalTask(r);
            }
        }

        bool Preempt() override {
            auto* state = LocalState;
            if (state && state->Executor == this) [[likely]] {
                return TClock::now() >= state->PreemptDeadline;
            }
            // Preempt all unexpected threads
            return true;
        }

    private:
        void InitSeed(TThreadState* state) noexcept {
            auto now = std::chrono::high_resolution_clock::now();
            auto a = now.time_since_epoch().count();
            auto b = std::this_thread::get_id();
            auto c = std::hash<decltype(b)>()(b);
            state->Random.seed(a + c);
        }

        void RunWorker(TThreadState* state) noexcept {
            InitSeed(state);

            LocalState = state;

            while (IRunnable* r = NextTask(state)) {
                state->PreemptDeadline = TClock::now() + Settings.PreemptEvery;
                do {
                    r = r->Run();
                } while (r);
            }

            LocalState = nullptr;
        }

        IRunnable* NextTask(TThreadState* state) noexcept {
            // Check global queue periodically
            if (state->LocalProcessed >= 61) {
                state->LocalProcessed = 0;
                if (IRunnable* r = NextGlobalTask(state)) {
                    return r;
                }
            }

            if (IRunnable* r = NextLocalTask(state)) {
                state->LocalProcessed++;
                return r;
            }

            if (IRunnable* r = TryStealing(state)) {
                state->LocalProcessed++;
                return r;
            }

            // We couldn't find anything fast, block waiting for various wakeup conditions
            std::unique_lock g(Mutex);

            // Try to see if we have anything in the global queue
            if (IRunnable* r = NextGlobalTaskLocked(state)) {
                return r;
            }

            // Try to steal tasks as long as we have the FlagLocalWork bits set
            auto current = BlockedThreads.load(std::memory_order_seq_cst);
            while (current & FlagLocalWork) {
                if (IRunnable* r = TryStealing(state)) {
                    state->LocalProcessed++;
                    return r;
                }
                uint32_t decrement = (current & FlagLocalWorkHigh) ? FlagLocalWorkHigh : FlagLocalWorkLow;
                current = BlockedThreadsSub(decrement, std::memory_order_seq_cst);
            }

            // Start waiting for more work to arrive
            IRunnable* r = nullptr;
            bool notified = false;
            current = BlockedThreadsAdd(WaitingThreadsIncrement, std::memory_order_seq_cst);
            for (;;) {
                while (current & FlagLocalWork) {
                    r = TryStealing(state);
                    if (r) {
                        state->LocalProcessed++;
                        break;
                    }
                    uint32_t decrement = (current & FlagLocalWorkHigh) ? FlagLocalWorkHigh : FlagLocalWorkLow;
                    current = BlockedThreadsSub(decrement, std::memory_order_seq_cst);
                }

                if (r) {
                    break;
                }

                if (current & FlagShutdown) {
                    r = nullptr;
                    break;
                }

                WakeUp.wait(g);
                notified = true;

                r = NextGlobalTaskLocked(state);
                if (r) {
                    break;
                }

                current = BlockedThreads.load(std::memory_order_seq_cst);
            }

            current = BlockedThreadsSub(WaitingThreadsIncrement, std::memory_order_seq_cst);
            if (notified && (current & FlagWakeupMask) && current >= WaitingThreadsIncrement) {
                // Note: we still have work bits set, wake the next thread
                WakeUp.notify_one();
            }

            return r;
        }

        uint32_t BlockedThreadsAdd(uint32_t increment, std::memory_order order) noexcept {
            return BlockedThreads.fetch_add(increment, order) + increment;
        }

        uint32_t BlockedThreadsSub(uint32_t decrement, std::memory_order order) noexcept {
            return BlockedThreads.fetch_sub(decrement, order) - decrement;
        }

    private:
        void PushGlobalTask(IRunnable* r) {
            r->Link[0].store(nullptr, std::memory_order_relaxed);
            std::unique_lock g(Mutex);
            if (GlobalQueueTail) {
                GlobalQueueTail->Link[0].store(r, std::memory_order_relaxed);
                GlobalQueueTail = r;
            } else {
                GlobalQueueHead = r;
                GlobalQueueTail = r;
            }
            WakeByGlobalTasksLocked();
        }

        void PushGlobalTaskBatch(IRunnable* head, IRunnable* tail) {
            std::unique_lock g(Mutex);
            if (GlobalQueueTail) {
                GlobalQueueTail->Link[0].store(head, std::memory_order_relaxed);
                GlobalQueueTail = tail;
            } else {
                GlobalQueueHead = head;
                GlobalQueueTail = tail;
            }
            WakeByGlobalTasksLocked();
        }

        void WakeByGlobalTasksLocked() {
            auto current = BlockedThreads.load(std::memory_order_seq_cst);
            if (!(current & FlagGlobalWork)) {
                BlockedThreads.fetch_or(FlagGlobalWork, std::memory_order_seq_cst);
                // We have waiting threads and this is the first time new work appeared
                if ((current & FlagWakeupMask) == 0 && current >= WaitingThreadsIncrement) {
                    WakeUp.notify_one();
                }
            }
        }

        IRunnable* NextGlobalTaskLocked(TThreadState* state) {
            if (!GlobalQueueHead) {
                return nullptr;
            }

            IRunnable* r = GlobalQueueHead;
            GlobalQueueHead = reinterpret_cast<IRunnable*>(r->Link[0].load(std::memory_order_relaxed));
            if (!GlobalQueueHead) {
                GlobalQueueTail = nullptr;
                BlockedThreads.fetch_and(~FlagGlobalWork, std::memory_order_seq_cst);
            }

            // It is possible that local work was deferred, but we have taken
            // a global task instead. Make sure we mark local work as present
            // and wake any waiters when necessary.
            if (LocalQueueSize(state) > 0) {
                auto current = BlockedThreads.load(std::memory_order_seq_cst);
                if ((current & FlagLocalWork) == 0) {
                    auto prev = BlockedThreads.fetch_or(FlagLocalWork, std::memory_order_seq_cst);
                    if ((prev & FlagWakeupMask) == 0 && prev >= WaitingThreadsIncrement) {
                        WakeUp.notify_one();
                    }
                }
            }

            return r;
        }

        IRunnable* NextGlobalTask(TThreadState* state) {
            // Avoid locking the mutex when there is no work in the global queue
            if (BlockedThreads.load(std::memory_order_seq_cst) & FlagGlobalWork) {
                std::unique_lock g(Mutex);
                return NextGlobalTaskLocked(state);
            } else {
                return nullptr;
            }
        }

    private:
        static uint64_t PackHeadTail(uint32_t head, uint32_t tail) noexcept {
            return (uint64_t(head) << 32) | tail;
        }

        static std::tuple<uint32_t, uint32_t> UnpackHeadTail(uint64_t value) noexcept {
            return { uint32_t(value >> 32), uint32_t(value) };
        }

        IRunnable* NextLocalTask(TThreadState* state) noexcept {
            auto head_tail = state->LocalQueueHeadTail.load(std::memory_order_relaxed);
            for (;;) {
                auto [head, tail] = UnpackHeadTail(head_tail);
                if (head == tail) {
                    return nullptr;
                }
                IRunnable* r = state->LocalQueue[head & TaskIndexMask].load(std::memory_order_relaxed);
                // Note: we don't need any synchronization here, because only the
                // current thread ever writes to the local queue and reordering
                // should be ok.
                if (state->LocalQueueHeadTail.compare_exchange_strong(head_tail,
                        PackHeadTail(head + 1, tail), std::memory_order_relaxed))
                {
                    return r;
                }
                // Lost the race with a stealer, head_tail is reloaded
            }
        }

        bool OffloadLocalTasks(TThreadState* state) noexcept {
            auto head_tail = state->LocalQueueHeadTail.load(std::memory_order_relaxed);
            for (;;) {
                auto [head, tail] = UnpackHeadTail(head_tail);
                uint32_t n = tail - head;
                if (n < MaxLocalTasks) {
                    break;
                }
                IRunnable* tasks[MaxLocalTasks / 2];
                n = MaxLocalTasks / 2;
                for (uint32_t i = 0; i < n; ++i) {
                    tasks[i] = state->LocalQueue[(head + i) & TaskIndexMask].load(std::memory_order_relaxed);
                }
                // Note: we don't need any synchronization here, because only the
                // current thread ever writes to the local queue and reordering
                // should be ok.
                if (state->LocalQueueHeadTail.compare_exchange_strong(head_tail,
                        PackHeadTail(head + n, tail), std::memory_order_relaxed))
                {
                    for (uint32_t i = 1; i < n; ++i) {
                        tasks[i-1]->Link[0].store(tasks[i], std::memory_order_relaxed);
                    }
                    tasks[n-1]->Link[0].store(nullptr, std::memory_order_relaxed);
                    PushGlobalTaskBatch(tasks[0], tasks[n-1]);
                    return true;
                }
                // Lost the race with a stealer, head_tail is reloaded
            }
            return false;
        }

        bool PushLocalTask(TThreadState* state, IRunnable* r) noexcept {
            // Note: tail is only modified locally, no synchronization needed
            // And while head may be updated by a stealer, we only use it for
            // determining if there's enough capacity, nothing else.
            auto head_tail = state->LocalQueueHeadTail.load(std::memory_order_relaxed);
            auto [head, tail] = UnpackHeadTail(head_tail);
            if (uint32_t(tail - head) >= MaxLocalTasks) {
                return false;
            }
            state->LocalQueue[tail & TaskIndexMask].store(r, std::memory_order_relaxed);
            // We want to increment tail without changing head as a single increment
            // This computes a wrapping difference that will make tail = tail + 1
            // even when other threads are modifying head concurrently.
            uint64_t increment = PackHeadTail(0, tail + 1) - PackHeadTail(0, tail);
            // Note: release synchronizes with other threads stealing tasks
            // Note: we need seq_cst to establish order with loading FlagLocalWork
            state->LocalQueueHeadTail.fetch_add(increment, std::memory_order_seq_cst);
            return true;
        }

        uint32_t LocalQueueSize(TThreadState* state) noexcept {
            auto head_tail = state->LocalQueueHeadTail.load(std::memory_order_relaxed);
            auto [head, tail] = UnpackHeadTail(head_tail);
            return tail - head;
        }

        IRunnable* StealFromLocal(TThreadState* state, TThreadState* from) noexcept {
            auto our_head_tail = state->LocalQueueHeadTail.load(std::memory_order_relaxed);
            auto [our_head, our_tail] = UnpackHeadTail(our_head_tail);
            assert(our_head == our_tail);
            // Note: acquire synchronizes with PushLocalTask
            // Note: we use seq_cst to establish order with FlagLocalWork decrement
            auto their_head_tail = from->LocalQueueHeadTail.load(std::memory_order_seq_cst);
            for (;;) {
                auto [their_head, their_tail] = UnpackHeadTail(their_head_tail);
                uint32_t n = their_tail - their_head;
                if (n == 0) {
                    return nullptr;
                }
                n -= n >> 1;
                // Copy pointer values, the first task will be returned
                auto* r = from->LocalQueue[their_head & TaskIndexMask].load(std::memory_order_relaxed);
                for (uint32_t i = 1; i < n; ++i) {
                    auto* more = from->LocalQueue[(their_head + i) & TaskIndexMask].load(std::memory_order_relaxed);
                    state->LocalQueue[(our_tail + i - 1) & TaskIndexMask].store(more, std::memory_order_relaxed);
                }
                // Note: acquire needed in case of failures
                // Note: release needed so loads above are not reordered with cas
                // Note: we use seq_cst for consistency here
                if (from->LocalQueueHeadTail.compare_exchange_strong(their_head_tail,
                        PackHeadTail(their_head + n, their_tail), std::memory_order_seq_cst))
                {
                    // We successfully stole some tasks
                    if (n > 1) {
                        // Note: we can use a store, because local queue was empty
                        // and no concurrent stealer could have changed head/tail
                        // Note: release synchronizes with other threads stealing our tasks
                        // Note: we use seq_cst for consistency here
                        state->LocalQueueHeadTail.store(
                            PackHeadTail(our_head, our_tail + n - 1),
                            std::memory_order_seq_cst);
                    }
                    return r;
                }
                // Lost the race: will retry with updated their_head_tail
            }
        }

        IRunnable* TryStealing(TThreadState* state) noexcept {
            uint32_t pos = state->Random();
            for (size_t i = 0; i < Settings.MaxThreads; ++i, pos += 1) {
                TThreadState* from = &ThreadStates[pos % Settings.MaxThreads];
                if (state != from) {
                    if (auto* r = StealFromLocal(state, from)) {
                        return r;
                    }
                }
            }
            return nullptr;
        }

        void WakeByLocalTasks() noexcept {
            // Note: we use seq_cst to establish order with regards to PushLocalTask
            auto current = BlockedThreads.load(std::memory_order_seq_cst);
            if ((current & FlagLocalWork) != FlagLocalWork) {
                auto prev = BlockedThreads.fetch_or(FlagLocalWork, std::memory_order_seq_cst);
                if ((prev & FlagWakeupMask) == 0 && prev >= WaitingThreadsIncrement) {
                    std::unique_lock g(Mutex);
                    current = BlockedThreads.load(std::memory_order_relaxed);
                    if (current >= WaitingThreadsIncrement) {
                        WakeUp.notify_one();
                    }
                }
            }
        }

    private:
        static constexpr uint32_t FlagShutdown = 1;
        static constexpr uint32_t FlagLocalWork = 2 + 4;
        static constexpr uint32_t FlagLocalWorkLow = 2;
        static constexpr uint32_t FlagLocalWorkHigh = 4;
        static constexpr uint32_t FlagGlobalWork = 8;

        // When any of these are set we need to wake up threads
        static constexpr uint32_t FlagWakeupMask = 1 + 2 + 4 + 8;

        static constexpr int WaitingThreadsShift = 4;
        static constexpr uint32_t WaitingThreadsIncrement = uint32_t(1) << WaitingThreadsShift;

    private:
        TWorkStealingExecutorSettings Settings;
        std::deque<TThreadState> ThreadStates;
        std::deque<std::thread> Threads;

        alignas(128) std::mutex Mutex;
        std::condition_variable WakeUp;
        IRunnable* GlobalQueueHead{ nullptr };
        IRunnable* GlobalQueueTail{ nullptr };

        std::atomic<uint32_t> BlockedThreads{ 0 };

    private:
        static inline thread_local TThreadState* LocalState{ nullptr };
    };

    std::shared_ptr<IExecutor> CreateWorkStealingExecutor(const TWorkStealingExecutorSettings& settings) {
        return std::make_shared<TWorkStealingExecutor>(settings);
    }

} // namespace fiberactors
