# fiberactors

Experimental actors and structured concurrency based on Boost.Context fibers.

## Preface

This is a spin-off of [coroactors](https://github.com/snaury/coroactors), where I experimented with structured concurrency and Swift-like actors based on C++ coroutines. Unfortunately async functions using C++ coroutines seem to be a bad choice for performance-critical code right now:

* Bad performance. Overhead of one async function calling another async function can be 100x that of a normal function call. While this is not unique to C++ coroutines (similar overhead was observed in Swift when calling async functions without inlining), it is much worse with C++ coroutines, because there's practically no inlining (compared to Swift), and complex implementation leads to poor optimization.
* Bugs in compilers. Bugs that require workarounds leading to poor performance. Examples include local variables leaking from awaiter to coroutine frame, which lead to races during cross-thread wakeups, or symmetric transfer based on tailcalls, which may not be available at all optimization levels, leading to code potentially blowing up due to stack overflow.
* Thread local variable problems. Initially I was looking at C++ coroutines since they were supposed to have very good support for thread-locals (await_suspend is a normal function call after all), so they should have been safe to migrate between threads. In practice it is far from the case, compiled code sometimes assumes thread local addresses don't change between suspend and resume, and leads to unexpected bugs in inlined code. This is a common problem with fibers, and shows how currently C++ coroutines can have vastly worse performance with little benefit.

Given context switch between Boost.Context fibers can cost less than a single async function call, it may be interesting to experiment with a structured concurrency library based on fibers. One advantage would be that normal function calls will continue to be normal function calls, and context switches are only needed when fiber has to really wait for something, in which case overhead is a small concern anyway.

## Design

See [DESIGN.md](DESIGN.md)
