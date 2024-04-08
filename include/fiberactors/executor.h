#pragma once
#include <atomic>
#include <memory>

namespace fiberactors {

    static_assert(sizeof(uintptr_t) == sizeof(void*),
        "This library assumes uintptr_t is the same size as a pointer");

    /**
     * Used to represent type-erased units of work in the executor
     */
    struct TRunnable {
        using TRunPtr = TRunnable* (*)(TRunnable*) noexcept;

        /**
         * Called to perform a unit of work.
         *
         * May return a pointer to another runnable, which may run immediately
         * as an efficient form of tail call. Such tail calls will share the
         * time slice of the original runnable.
         *
         * Represented as a function pointer instead of a virtual method for
         * efficiency (avoids an extra indirection for a method lookup), and
         * to support dynamically changing state between method calls. This
         * function takes a runnable pointer as the first argument, which
         * may be converted to a derived class pointer when needed.
         */
        TRunPtr RunPtr;

        /**
         * Used for bookkeeping (intrusive lists) without extra allocations
         *
         * Intentionally uninitialized. Uses uintptr_t instead of pointers,
         * because special marker values may be used in certain situations.
         */
        std::atomic<uintptr_t> LinkPtr[2];

        /**
         * The default constructor
         */
        TRunnable() = default;

        /**
         * Initializes runnable with the specified run pointer
         */
        TRunnable(TRunPtr run) noexcept
            : RunPtr(run)
        {}

        /**
         * Helps converting methods to plain functions
         */
        template<auto RunMethod>
        struct TRunMethodWrapper;

        /**
         * Helps converting a derived class method to a plain function
         */
        template<class TDerived, TRunnable* (TDerived::*RunMethod)() noexcept>
        struct TRunMethodWrapper<RunMethod> {
            static TRunnable* Run(TRunnable* self) noexcept {
                return (static_cast<TDerived*>(self)->*RunMethod)();
            }
        };

        /**
         * Converts a derived class method (&TDerived::Run) to a plain TRunPtr
         */
        template<auto RunMethod>
        static inline constexpr TRunPtr MethodRunPtr = TRunMethodWrapper<RunMethod>::Run;
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
        virtual void Post(TRunnable*) = 0;

        /**
         * Used to defer a runnable to the executor, which will eventually call
         * the `Run` method. May only be used when logical thread of execution
         * does not work, and this runnable is a continuation of whatever
         * activity is currently running. The primary effect is that unlike
         * `Post` this will add the runnable to the queue, but may not wake up
         * any additional threads.
         */
        virtual void Defer(TRunnable*) = 0;

        /**
         * May be used to check whether current activity should be preempted.
         *
         * The default implementation always returns false.
         */
        virtual bool Preempt() {
            return false;
        }
    };

    /**
     * A runnable that is bound to a specific executor
     */
    struct TRunnableWithExecutor : public TRunnable {
        /**
         * The executor that this runnable is bound to.
         */
        IExecutor* const Executor;

        explicit TRunnableWithExecutor(IExecutor* executor)
            : Executor(executor)
        {}

        explicit TRunnableWithExecutor(IExecutor* executor, TRunPtr run)
            : TRunnable(run)
            , Executor(executor)
        {}
    };

} // namespace fiberactors
