#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "threading/ThreadPool.hpp"

using search::threading::ThreadPool;
using search::threading::ThreadPoolStoppedError;
using namespace std::chrono_literals;

TEST(ThreadPoolTest, RejectsZeroThreads) {
    EXPECT_THROW(ThreadPool pool(0), std::invalid_argument);
}

TEST(ThreadPoolTest, CreatesRequestedNumberOfWorkers) {
    ThreadPool pool(5);
    EXPECT_EQ(pool.size(), 5u);
}

TEST(ThreadPoolTest, SubmitReturnsResult) {
    ThreadPool pool(2);
    auto future = pool.submit([](int a, int b) { return a + b; }, 20, 22);
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolTest, ExecutesAllTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    constexpr int kTasks = 10000;
    for (int i = 0; i < kTasks; ++i) {
        pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.waitAll();
    EXPECT_EQ(counter.load(), kTasks);
    EXPECT_EQ(pool.completedTasks(), static_cast<std::size_t>(kTasks));
}

TEST(ThreadPoolTest, UsesMultipleThreads) {
    ThreadPool pool(4);
    std::mutex mutex;
    std::set<std::thread::id> ids;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 64; ++i) {
        futures.push_back(pool.submit([&] {
            std::this_thread::sleep_for(2ms);
            std::lock_guard lock(mutex);
            ids.insert(std::this_thread::get_id());
        }));
    }
    for (auto& f : futures) f.get();
    EXPECT_GT(ids.size(), 1u);
    EXPECT_LE(ids.size(), 4u);
}

TEST(ThreadPoolTest, ExceptionIsPropagatedThroughFuture) {
    ThreadPool pool(2);
    auto future = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(future.get(), std::runtime_error);
    // The worker survived the exception and still executes tasks.
    EXPECT_EQ(pool.submit([] { return 7; }).get(), 7);
}

TEST(ThreadPoolTest, PostExceptionIsCountedAndReported) {
    std::atomic<int> reported{0};
    ThreadPool pool(2, [&reported](const std::exception_ptr&) { ++reported; });
    pool.post([] { throw std::logic_error("fail"); });
    pool.post([] { throw 42; });
    std::atomic<bool> ran{false};
    pool.post([&ran] { ran = true; });
    pool.waitAll();
    EXPECT_EQ(pool.failedTasks(), 2u);
    EXPECT_EQ(reported.load(), 2);
    EXPECT_TRUE(ran.load());
}

TEST(ThreadPoolTest, RejectsTasksAfterShutdown) {
    ThreadPool pool(2);
    pool.shutdown();
    EXPECT_TRUE(pool.isStopped());
    EXPECT_THROW(pool.post([] {}), ThreadPoolStoppedError);
    EXPECT_THROW(pool.submit([] { return 1; }), ThreadPoolStoppedError);
}

TEST(ThreadPoolTest, ShutdownDrainsQueuedTasks) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(1);
        for (int i = 0; i < 100; ++i) {
            pool.post([&counter] {
                std::this_thread::sleep_for(100us);
                ++counter;
            });
        }
        pool.shutdown();  // must finish all 100 queued tasks
    }
    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPoolTest, DestructorJoinsWorkers) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(3);
        for (int i = 0; i < 30; ++i) {
            pool.post([&counter] { ++counter; });
        }
    }  // destructor == shutdown + join
    EXPECT_EQ(counter.load(), 30);
}

TEST(ThreadPoolTest, ShutdownIsIdempotentAndThreadSafe) {
    ThreadPool pool(4);
    std::vector<std::thread> stoppers;
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&pool] { pool.shutdown(); });
    }
    for (auto& t : stoppers) t.join();
    pool.shutdown();
    EXPECT_TRUE(pool.isStopped());
}

TEST(ThreadPoolTest, WaitAllOnIdlePoolReturnsImmediately) {
    ThreadPool pool(2);
    pool.waitAll();
    SUCCEED();
}

TEST(ThreadPoolTest, ConcurrentProducersDoNotLoseTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 2000;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto& t : producers) t.join();
    pool.waitAll();
    EXPECT_EQ(counter.load(), kProducers * kPerProducer);
}

TEST(ThreadPoolTest, TasksCanSubmitNestedTasks) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};
    auto outer = pool.submit([&] {
        for (int i = 0; i < 10; ++i) {
            pool.post([&counter] { ++counter; });
        }
    });
    outer.get();
    pool.waitAll();
    EXPECT_EQ(counter.load(), 10);
}

TEST(ThreadPoolTest, ParallelExecutionIsFasterThanSequential) {
    // 8 tasks of 50 ms on 8 workers must take far less than 400 ms.
    ThreadPool pool(8);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submit([] { std::this_thread::sleep_for(50ms); }));
    }
    for (auto& f : futures) f.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 300ms);
}
