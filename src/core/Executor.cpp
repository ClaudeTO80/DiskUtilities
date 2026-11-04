/**
 * @file Executor.cpp
 * @brief StdThreadPool implementation.
 * @details A mutex-protected FIFO queue drained by std::jthread workers. The destructor lets the workers finish
 *          every queued task before joining them.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/core/Executor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace diskutilities {

StdThreadPool::StdThreadPool(std::size_t threads) {
    if (threads == 0) threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    workers_.reserve(threads);
    try {
        for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
    } catch (...) {
        // Thread creation failed half-way: stop the workers already started
        // before propagating, otherwise they would wait forever.
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        throw;
    }
}

StdThreadPool::~StdThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // std::jthread joins in its destructor; workers drain the queue first.
}

void StdThreadPool::post(Task task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) throw std::logic_error("StdThreadPool: post() dopo la chiusura del pool");
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void StdThreadPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return; // stopping_ and nothing left to do
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task();
        } catch (...) {
            // Deliberately ignored: see class documentation.
        }
    }
}

} // namespace diskutilities
