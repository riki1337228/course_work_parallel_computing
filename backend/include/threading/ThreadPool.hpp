#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace search::threading {

/// Thrown by submit()/post() after shutdown() has been requested.
class ThreadPoolStoppedError : public std::runtime_error {
public:
    ThreadPoolStoppedError() : std::runtime_error("thread pool is stopped") {}
};

/// Fixed-size pool of worker threads with a shared FIFO task queue.
///
/// Synchronization:
///  - `mutex_` protects `tasks_`, `stopping_` and `activeTasks_`;
///  - `taskAvailable_` wakes workers when a task is queued or on shutdown;
///  - `allDone_` wakes waitAll() callers when the queue is empty and no task runs.
///
/// Guarantees:
///  - tasks are executed outside the lock (no lock is held while user code runs);
///  - exceptions thrown by a task never terminate a worker: for submit() they are
///    delivered through the returned std::future, for post() they are counted
///    (failedTasks()) and reported to an optional error handler;
///  - shutdown() stops accepting new tasks, drains the queue and joins all workers;
///  - the destructor calls shutdown(), so no thread is ever leaked.
class ThreadPool {
public:
    using ErrorHandler = std::function<void(const std::exception_ptr&)>;

    /// Creates `threadCount` workers (must be > 0, otherwise std::invalid_argument).
    explicit ThreadPool(std::size_t threadCount, ErrorHandler onError = {});
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Enqueues a callable and returns a future with its result (or exception).
    template <typename F, typename... Args>
    auto submit(F&& func, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [f = std::forward<F>(func),
             tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
                return std::apply(std::move(f), std::move(tuple));
            });
        std::future<Result> future = task->get_future();
        enqueue([task]() { (*task)(); });
        return future;
    }

    /// Enqueues a fire-and-forget task. Exceptions are caught and counted.
    void post(std::function<void()> task);

    /// Blocks until the queue is empty and no task is being executed.
    void waitAll();

    /// Stops accepting tasks, finishes all queued tasks and joins the workers.
    /// Idempotent and safe to call from several threads (but not from a worker).
    void shutdown();

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
    [[nodiscard]] std::size_t pendingTasks() const;
    [[nodiscard]] std::size_t activeTasks() const;
    [[nodiscard]] std::size_t completedTasks() const noexcept { return completed_.load(); }
    [[nodiscard]] std::size_t failedTasks() const noexcept { return failed_.load(); }
    [[nodiscard]] bool isStopped() const;

private:
    void enqueue(std::function<void()> task);
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable taskAvailable_;
    std::condition_variable allDone_;
    std::size_t activeTasks_ = 0;
    bool stopping_ = false;

    std::once_flag joinOnce_;
    std::atomic<std::size_t> completed_{0};
    std::atomic<std::size_t> failed_{0};
    ErrorHandler onError_;
};

}  // namespace search::threading
