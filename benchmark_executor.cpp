#include <fiberactors/executor_work_stealing.h>
#include <deque>
#include <iostream>
#include <chrono>
#include <thread>

using namespace fiberactors;

class alignas(128) ExecutorThroughputTask final
    : public TRunnable
{
    static_assert(sizeof(TRunPtr) == 8);

public:
    ExecutorThroughputTask(IExecutor* executor, bool usePost)
        : TRunnable(MethodRunPtr<&ExecutorThroughputTask::Run>)
        , executor(executor)
        , usePost(usePost)
    {}

    uint64_t GetCount() noexcept {
        return count_.exchange(0, std::memory_order_relaxed);
    }

private:
    TRunnable* Run() noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        if (usePost) {
            executor->Post(this);
        } else {
            executor->Defer(this);
        }
        return nullptr;
    }

private:
    IExecutor* executor;
    const bool usePost;
    std::atomic<uint64_t> count_{ 0 };
};

int main(int argc, char** argv) {
    int numTasks = 1;
    int numThreads = 1;
    bool usePost = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "-p" || arg == "--tasks") && i + 1 < argc) {
            numTasks = std::stoi(argv[++i]);
            continue;
        }
        if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
            continue;
        }
        if (arg == "--use-post") {
            usePost = true;
            continue;
        }
        if (arg == "--use-defer") {
            usePost = false;
            continue;
        }
        std::cerr << "ERROR: unexpected argument " << arg << std::endl;
        return 1;
    }

    auto executor = CreateWorkStealingExecutor({
        .MaxThreads = size_t(numThreads),
    });

    std::deque<ExecutorThroughputTask> tasks;
    for (int i = 0; i < numTasks; ++i) {
        tasks.emplace_back(executor.get(), usePost);
    }

    for (auto& task : tasks) {
        executor->Post(&task);
    }

    std::cout << "Started " << tasks.size() << " tasks" << std::endl;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t sum = 0, min = 0, max = 0;
        for (size_t i = 0; i < tasks.size(); ++i) {
            auto count = tasks[i].GetCount();
            if (i == 0) {
                sum = count;
                min = count;
                max = count;
            } else {
                sum += count;
                min = std::min(min, count);
                max = std::max(max, count);
            }
        }
        std::cout << "... " << sum << "/s (min=" << min << "/s, max=" << max << "/s)" << std::endl;
    }

    return 0;
}
