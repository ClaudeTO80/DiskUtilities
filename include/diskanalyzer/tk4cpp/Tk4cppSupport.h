#pragma once

/**
 * @file Tk4cppSupport.h
 * @brief Adapters that plug TK4CPP modules (Thread, Utility) into DiskAnalyzer.
 * @details Compiled only when the TK4CPP package is available (DISKANALYZER_WITH_TK4CPP). The core library
 *          never depends on it: it only sees the Executor interface and plain option values.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskanalyzer/core/Executor.h"

#include <TK4CPP/ThreadPool/ThreadPool.h>

#include <cstdint>
#include <string>

namespace diskanalyzer::tk4cpp {

/**
 * @brief Executor backed by TK4CPP::TP::ThreadPool.
 * @details The pool has a fixed size (minThreads == maxThreads) because scan tasks are
 * short and numerous: dynamic scaling would only add thread churn. Destruction
 * performs TK4CPP's graceful shutdown (queued tasks are run first).
 */
class Tk4cppExecutor final : public Executor {
public:
    /**
     * @brief Create a fixed-size pool.
     * @param threads Worker count; 0 selects recommendedScanThreads().
     * @param name    Pool name, used by TK4CPP for logging and thread names.
     */
    explicit Tk4cppExecutor(unsigned threads = 0, std::string name = "DiskScan");

    /**
     * @brief Submit @p task to the TK4CPP pool.
     * @param task Work to run.
     * @throws std::runtime_error if the pool refuses the task (not running, queue full).
     */
    void post(Task task) override;
    /** @brief Number of worker threads of the pool. */
    [[nodiscard]] std::size_t concurrency() const noexcept override { return threads_; }

    /** @brief Counters of the underlying pool (completed, failed, queued...). */
    [[nodiscard]] TK4CPP::TP::PoolStats stats() const noexcept { return pool_.stats(); }

private:
    static TK4CPP::TP::ThreadPoolConfig makeConfig(unsigned threads, std::string name);

    std::size_t threads_;
    TK4CPP::TP::ThreadPool pool_;
};

/**
 * @brief Worker count for a scan: physical cores (TK4CPP::Utility::System), clamped to [1, 8].
 * @details Directory scanning is bound by filesystem metadata latency, not by ALU work,
 * so hyper-threads add little and more than a handful of workers just contend.
 */
[[nodiscard]] unsigned recommendedScanThreads();

/**
 * @brief Entry limit that keeps a scan within the memory currently available.
 * @details Uses TK4CPP::Utility::System::freeMemory() and assumes a conservative cost per
 * entry (the scan result plus the indexed snapshot, see kBytesPerNode). Returns
 * 0 ("no limit") if the available memory cannot be determined.
 */
[[nodiscard]] std::uint64_t recommendedNodeLimit();

/** @brief Conservative memory cost of one entry, in bytes (tree node + snapshot indexes + name). */
inline constexpr std::uint64_t kBytesPerNode = 256;

/** @brief Human readable size ("1.23 GB") through TK4CPP::Utility::FolderHandler. */
[[nodiscard]] std::string formatBytes(std::uint64_t bytes);

} // namespace diskanalyzer::tk4cpp
