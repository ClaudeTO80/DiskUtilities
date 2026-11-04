#pragma once

/**
 * @file ManualExecutor.h
 * @brief Executor whose tasks run only when the test says so.
 * @details Makes "the scan is half done" deterministic: the test releases one directory task at a time and
 *          inspects the partial results in between.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/core/Executor.h"

#include <deque>
#include <mutex>
#include <utility>

/** @brief Queue-only executor; tasks run on whichever thread calls runOne(). */
class ManualExecutor final : public diskutilities::Executor {
public:
    /** @brief Remember @p task; it does not run until runOne(). */
    void post(Task task) override {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    /** @brief Always 1: tasks never run in parallel. */
    [[nodiscard]] std::size_t concurrency() const noexcept override { return 1; }

    /**
     * @brief Run the oldest queued task on the calling thread.
     * @return False if the queue was empty.
     */
    bool runOne() {
        Task task;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) return false;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
        return true;
    }

private:
    std::mutex mutex_;          ///< Guards #queue_.
    std::deque<Task> queue_;    ///< Pending tasks, oldest first.
};
