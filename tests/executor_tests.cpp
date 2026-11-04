/**
 * @file executor_tests.cpp
 * @brief Tests for StdThreadPool.
 * @details Covers ordering guarantees, exception isolation and orderly shutdown.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/core/Executor.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>

using diskutilities::StdThreadPool;

void runsEveryTask() {
    std::atomic<int> sum{0};
    {
        StdThreadPool pool(4);
        CHECK_EQ(pool.concurrency(), std::size_t{4});
        for (int i = 1; i <= 10000; ++i) pool.post([&sum, i] { sum += i; });
    } // destructor drains the queue
    CHECK_EQ(sum.load(), 10000 * 10001 / 2);
}

void nestedPostsWork() {
    std::atomic<int> count{0};
    std::latch done(1 + 100);
    StdThreadPool pool(2);
    pool.post([&] {
        for (int i = 0; i < 100; ++i) pool.post([&] { ++count; done.count_down(); });
        done.count_down();
    });
    done.wait();
    CHECK_EQ(count.load(), 100);
}

void defaultThreadCountIsPositive() {
    StdThreadPool pool(0);
    CHECK(pool.concurrency() >= 1);
}

void exceptionsDoNotKillWorkers() {
    std::atomic<int> after{0};
    {
        StdThreadPool pool(1);
        pool.post([] { throw std::runtime_error("boom"); });
        pool.post([] { throw 42; });
        pool.post([&] { ++after; });
    }
    CHECK_EQ(after.load(), 1);
}

void destructorDrainsSlowTasks() {
    std::atomic<int> ran{0};
    {
        StdThreadPool pool(2);
        for (int i = 0; i < 50; ++i)
            pool.post([&] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); ++ran; });
    }
    CHECK_EQ(ran.load(), 50);
}

void parallelismIsReal() {
    // Two tasks that wait for each other can only finish with >= 2 workers.
    StdThreadPool pool(2);
    std::latch both(2);
    std::atomic<int> finished{0};
    for (int i = 0; i < 2; ++i)
        pool.post([&] { both.arrive_and_wait(); ++finished; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (finished.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_EQ(finished.load(), 2);
}

int main() {
    RUN(runsEveryTask);
    RUN(nestedPostsWork);
    RUN(defaultThreadCountIsPositive);
    RUN(exceptionsDoNotKillWorkers);
    RUN(destructorDrainsSlowTasks);
    RUN(parallelismIsReal);
    return testReport("executor_tests");
}
