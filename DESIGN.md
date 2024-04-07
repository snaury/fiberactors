# Design of fiberactors

I know of a number of existing fiber libraries for C++:

* Boost.Fiber based on Boost.Context
* [Google Marl](https://github.com/google/marl)
* [Yandex userver](https://userver.tech/)

Unfortunately they are either too low-level, or too tightly coupled, and don't tackle the problem of safe concurrent access to shared data. What I think is needed is a structured concurrency framework, with direct support for working in isolation domains (actors). Unfortunately, existing libraries don't seem to extensible enough to add this support after the fact.

## Executors

I want executors (or schedulers) to be separate from fibers. But what I want is a way to schedule cross-thread work efficiently and without allocation. One way would be to support it via these interfaces:

```c++
class runnable : public intrusive_runnable_list_item {
public:
    virtual runnable* run() noexcept = 0;
};

class executor {
public:
    virtual void post(runnable* r) = 0;
    virtual void defer(runnable* r) = 0;
}
```

There are two important points:

* Runnable is an intrusive item, it could be added to runnable lists atomically without allocations.
* Fiber tasks would implement this interface, which would resume the fiber, and conditionally perform additional steps when the fiber suspends.
* Runnable may return itself or another runnable to support "tail call" transfers, e.g. from one fiber to another.

## Fibers

What some fiber-based frameworks seem to be missing, is that properly implemented fibers shouldn't need any locks, just like you don't need locks in normal functions when you don't access data shared between multiple threads. Fibers always run in a single thread. When suspended (due to an underlying wait for some async result) they may be resumed in another thread, but you only need to ensure it happens exactly once. Most of the time underlying async results already guarantee the callback will be called exactly once, which translates to fibers not needing any locks or atomics to suspend/resume. This will be safe as long as any work of arranging the fiber to resume will be performed after the fiber is already suspended.

I'm thinking of something like this:

```c++
class task : ... {
public:
    template<class Callback>
    static void suspend(Callback&& callback);

    // Example of a suspending function
    static void sleep(int microseconds);
};

// Example of a suspending function
void task:sleep(int microseconds) {    
    task::suspend([microseconds](runnable* r) noexcept -> runnable* {
        // Fiber is now suspended and this lambda is running on a "system" stack
        if (microseconds <= 0) {
            // Example where we might decide to immediately resume without rescheduling
            return r;
        }

        // ... schedule `r` to resume after `microseconds`

        // We return nullptr to indicate we don't need to transfer to another runnable
        return nullptr;
    });
}
```

The resume loop would provide some space for such callbacks, which are move-constructed there without allocating from the heap. After the current fiber switches back to the resume loop (the `runnable::run` implementation) the provided callback may decide whether it wants to resume immediately, schedule to resume later, or resume concurrently somewhere else (e.g. in another thread).

This is similar to how `await_suspend` works in C++ coroutines, and makes suspending entirely generic, supporting a wide variaty of scenarios, ones I cannot even think of yet. At the same time it would be usable without any need for locks most of the time.

## Structured concurrency

It is possible to implement familiar counterparts to `std::mutex`, `std::conditional_variable`, etc. However, when multiple tasks interact, it is often difficult to make sure tasks use shared data correctly (e.g. other tasks don't use data via dangling references), avoiding deadlocks and performance bottlenecks may become a challenge similar to normal multi-threaded programming.

Structured concurrency may prove to be a better alternative:

* Child tasks are created using task groups
* Cancellation of the parent task propagates to child tasks
* Parent task waits until all child tasks stop executing before returning
* The latter point makes it possible to safely use "stack" variables in child tasks

An example:

```c++

void doWork() {
    // Stack variable used by multiple child tasks
    MyData data;
    // Perform two calculations in parallel with the same data
    int result = with_task_group<int>([&](task_group<int>& g) {
        g.add([&]{ return perform_one(data); });
        g.add([&]{ return perform_two(data); });
        // Return the first result, the other task will be cancelled
        return g.next();
    });
    // We only return here when both child tasks have finished, destroying data is now safe
}
```

## Actor isolation

Similar to coroactors, it will be possible to lock current task to an actor context, where only one task executes in a particular context at a time. Current context will also be automatically released when fiber suspends (or when a different context is acquired), and reacquired when fiber resumes (or current context guard goes out of scope).

In the simplest case actor context works like a mutex:

```c++

class Counter {
public:
    int get() const noexcept {
        std::lock_guard guard(context);
        return value_;
    }

    void set(int value) noexcept {
        std::lock_guard guard(context);
        value_ = value;
    }

    int increment() noexcept {
        std::lock_guard guard(context);
        return ++value_;
    }

private:
    actor_context context;
    int value_ = 0;
};
```

When current task suspends, current context needs to be released behind the scenes, and reacquired just before task is resumed. When reacquiring the context, we may block again, and I think it may be encoded using different implementations of runnables. I think this may be supported by generic support for hooks:

```
class suspend_hook {
public:
    // This method is called when the user calls task::suspend
    // The last suspend_hook::suspend in the chain suspends the fiber
    virtual void suspend() = 0;

    // This method is called after the fiber is suspended
    // The last suspend_hook::suspended in the chain calls the user-provided callback
    // Initially `r` is the runnable that resumes the fiber, but may be wrapped
    // to perform additional actions before resuming and this wrapper may be
    // passed down the chain instead of the original.
    virtual runnable* suspended(runnable* r) noexcept = 0;
};

class task {
public:
    static suspend_hook* get_suspend_hook() noexcept;
    static void set_suspend_hook(suspend_hook*) noexcept;
}
```

When `actor_context::lock()` successfully acquires the context, it could install itself at the top of suspend hooks, which would call the next hook with a wrapper runnable, restoring the context on the way back. However, since actor context is never nested, a generic implementation would also need to disable lower hooks, and could be more efficient as an integral part of the task suspend logic instead.
