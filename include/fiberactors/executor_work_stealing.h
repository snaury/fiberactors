#pragma once
#include <fiberactors/executor.h>
#include <chrono>

namespace fiberactors {

    struct TWorkStealingExecutorSettings {
        /**
         * The maximum number of threads in the executor
         */
        size_t MaxThreads = 1;

        /**
         * Specifies how often tasks should be preempted
         */
        std::chrono::microseconds PreemptEvery{ 10 };
    };

    /**
     * Creates a work-stealing executor with the specified maximum number of threads
     */
    std::shared_ptr<IExecutor> CreateWorkStealingExecutor(const TWorkStealingExecutorSettings& settings);

} // namespace fiberactors
