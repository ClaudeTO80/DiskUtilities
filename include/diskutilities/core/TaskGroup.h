#pragma once

/**
 * @file TaskGroup.h
 * @brief Completion tracking for a dynamic set of tasks posted to an Executor.
 * @details Shared by the DirectoryScanner and the FileSearcher. The counter is decremented under the same
 *          mutex the waiter uses, so the waiting thread can never observe "idle" and destroy the shared
 *          state while a worker is still inside done().
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace diskutilities {

/** @brief Counter of in-flight tasks with a timed wait for "all done". */
class TaskGroup {
public:
    /** @brief Register one task; call it *before* posting the task. */
    void add() noexcept { pending_.fetch_add(1, std::memory_order_acq_rel); }

    /** @brief Mark one registered task as finished (also when posting it failed). */
    void done() noexcept {
        std::lock_guard lock(mutex_);
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) idle_.notify_all();
    }

    /** @brief True when no task is in flight. */
    [[nodiscard]] bool idle() const noexcept { return pending_.load(std::memory_order_acquire) == 0; }

    /**
     * @brief Wait until idle or until @p timeout elapses.
     * @return True if the group is idle.
     */
    template <class Rep, class Period>
    bool waitFor(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        idle_.wait_for(lock, timeout, [&] { return idle(); });
        return idle();
    }

private:
    std::mutex mutex_;
    std::condition_variable idle_;
    std::atomic<std::size_t> pending_{0};
};

} // namespace diskutilities
