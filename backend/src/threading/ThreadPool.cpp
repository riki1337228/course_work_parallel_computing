#include "threading/ThreadPool.hpp"

namespace search::threading {

ThreadPool::ThreadPool(std::size_t threadCount, ErrorHandler onError) : onError_(std::move(onError)) {
    if (threadCount == 0) {
        throw std::invalid_argument("ThreadPool requires at least one thread");
    }
    workers_.reserve(threadCount);
    try {
        for (std::size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back(&ThreadPool::workerLoop, this);
        }
    } catch (...) {
        shutdown();  // join threads that were already started
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            throw ThreadPoolStoppedError();
        }
        tasks_.push(std::move(task));
    }
    taskAvailable_.notify_one();
}

void ThreadPool::post(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("ThreadPool::post: empty task");
    }
    enqueue([this, task = std::move(task)]() {
        try {
            task();
        } catch (...) {
            failed_.fetch_add(1, std::memory_order_relaxed);
            if (onError_) {
                try {
                    onError_(std::current_exception());
                } catch (...) {
                    // error handler must never kill a worker
                }
            }
        }
    });
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            taskAvailable_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return;  // stopping_ and nothing left to do
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            ++activeTasks_;
        }

        try {
            task();  // executed without holding the lock
        } catch (...) {
            // submit() tasks store exceptions in the future and post() tasks catch
            // them, so this is only a last-resort guard that keeps the worker alive.
            failed_.fetch_add(1, std::memory_order_relaxed);
        }
        completed_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard lock(mutex_);
            --activeTasks_;
            if (activeTasks_ == 0 && tasks_.empty()) {
                allDone_.notify_all();
            }
        }
    }
}

void ThreadPool::waitAll() {
    std::unique_lock lock(mutex_);
    allDone_.wait(lock, [this] { return tasks_.empty() && activeTasks_ == 0; });
}

void ThreadPool::shutdown() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    taskAvailable_.notify_all();
    std::call_once(joinOnce_, [this] {
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    });
}

std::size_t ThreadPool::pendingTasks() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

std::size_t ThreadPool::activeTasks() const {
    std::lock_guard lock(mutex_);
    return activeTasks_;
}

bool ThreadPool::isStopped() const {
    std::lock_guard lock(mutex_);
    return stopping_;
}

}  // namespace search::threading
