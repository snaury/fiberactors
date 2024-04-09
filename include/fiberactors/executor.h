#pragma once
#include <atomic>
#include <memory>

namespace fiberactors {

    static_assert(sizeof(uintptr_t) == sizeof(void*),
        "This library assumes uintptr_t is the same size as a pointer");

    class IRunnable;
    class IExecutor;

    /**
     * Used to represent type-erased units of work in the executor
     */
    class IRunnable {
    public:
        /**
         * Used for bookkeeping (intrusive lists) without extra allocations
         *
         * Intentionally uninitialized. Uses uintptr_t instead of pointers,
         * because special marker values may be used in certain situations.
         */
        std::atomic<uintptr_t> LinkPtr[2];

        /**
         * Called to perform a unit of work.
         *
         * May return a pointer to another runnable, which may run immediately
         * as an efficient form of tail call. Such tail calls will share the
         * time slice of the original runnable.
         */
        virtual IRunnable* Run() noexcept = 0;

    protected:
        ~IRunnable() = default;
    };

    /**
     * Used to represent multi-threaded executors
     */
    class IExecutor {
    public:
        virtual ~IExecutor() = default;

        /**
         * Used to post a runnable to the executor, which will eventually call
         * the `Run` method. Must be used when the logical thread of execution
         * forks, where the runnable should executes in parallel with current
         * activity.
         */
        virtual void Post(IRunnable*) = 0;

        /**
         * Used to defer a runnable to the executor, which will eventually call
         * the `Run` method. May only be used when logical thread of execution
         * does not work, and this runnable is a continuation of whatever
         * activity is currently running. The primary effect is that unlike
         * `Post` this will add the runnable to the queue, but may not wake up
         * any additional threads.
         */
        virtual void Defer(IRunnable*) = 0;

        /**
         * May be used to check whether current activity should be preempted.
         *
         * The default implementation always returns false.
         */
        virtual bool Preempt() {
            return false;
        }
    };

} // namespace fiberactors
