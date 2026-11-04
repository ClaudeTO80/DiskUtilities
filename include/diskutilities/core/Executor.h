#pragma once

/**
 * @file Executor.h
 * @brief Minimal task-execution abstraction used by the scanner.
 * @details The scanner never creates threads on its own: it posts work to an diskutilities::Executor. The
 *          default implementation is StdThreadPool, but any other pool (for instance the TK4CPP ThreadPool) can
 *          be plugged in by writing a thin adapter that implements this interface.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace diskutilities {

/**
 * @brief Abstract sink for units of work.
 * @details Contract for implementations:
 *  - post() may be called concurrently from any thread, including from a task
 *    that is already running on the executor;
 *  - tasks posted from inside a task must not be required to run before the
 *    posting task returns (the scanner never blocks a worker on another task);
 *  - post() may throw (for example std::bad_alloc); callers handle that.
 */
class Executor {
public:
    /** @brief Unit of work: a nullary callable without return value. */
    using Task = std::function<void()>;

    virtual ~Executor() = default;

    /**
     * @brief Schedule @p task for execution on some worker thread.
     * @param task Work to run; must be callable exactly once.
     */
    virtual void post(Task task) = 0;

    /** @brief Number of tasks that can run in parallel (always >= 1). */
    [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;
};

/**
 * @brief Fixed-size thread pool built on std::jthread.
 * @details Exceptions escaping a task are swallowed so a faulty task cannot terminate
 * the process; tasks are expected to report errors through their own channel.
 * The destructor runs every task still queued, then joins the workers.
 */
class StdThreadPool final : public Executor {
public:
    /** @param threads Worker count; 0 selects std::thread::hardware_concurrency(). */
    explicit StdThreadPool(std::size_t threads = 0);
    ~StdThreadPool() override;

    StdThreadPool(const StdThreadPool&) = delete;
    StdThreadPool& operator=(const StdThreadPool&) = delete;

    /**
     * @brief Queue @p task for execution.
     * @param task Work to run.
     * @throws std::logic_error if the pool is already shutting down.
     */
    void post(Task task) override;

    /** @brief Number of worker threads. */
    [[nodiscard]] std::size_t concurrency() const noexcept override { return workers_.size(); }

private:
    void workerLoop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> queue_;
    bool stopping_ = false;
    std::vector<std::jthread> workers_;
};

} // namespace diskutilities
