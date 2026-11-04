#pragma once

/**
 * @file ScanController.h
 * @brief Runs scans on a background thread and publishes finished snapshots.
 * @details At most one scan runs at a time. The controller exposes a copyable ScanStatus that the UI polls, and
 *          it publishes a new Snapshot to the SnapshotBridge only when a scan completes successfully.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/core/Executor.h"
#include "diskutilities/filesystem/DirectoryScanner.h"
#include "diskutilities/web/SnapshotBridge.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace diskutilities::web {

/** @brief Lifecycle of a scan as seen by the UI. */
enum class ScanState { Idle, Running, Completed, Cancelled, Failed };

/** @brief Stable lower-case name of a state ("idle", "running", ...), used by the UI. */
[[nodiscard]] const char* toString(ScanState state) noexcept;

/** @brief Observable state of the controller; a plain value, safe to copy across threads. */
struct ScanStatus {
    ScanState state = ScanState::Idle; ///< Current lifecycle state.
    std::string phase;       ///< While running: "scanning" or "indexing".
    std::string root;        ///< UTF-8 root of the current or last scan.
    std::string currentPath; ///< Directory being read right now (best effort).
    std::string error;       ///< Failure description when the state is Failed.
    std::uint64_t entriesVisited = 0;    ///< Entries examined so far.
    std::uint64_t filesFound = 0;        ///< Regular files found so far.
    std::uint64_t directoriesFound = 0;  ///< Directories found so far.
    std::uint64_t inaccessibleFound = 0; ///< Entries that could not be read.
    std::uint64_t elapsedMs = 0;         ///< Milliseconds since the scan started.
    std::uint64_t bytesFound = 0;        ///< Combined size of the regular files found so far.
    VolumeInfo volume;                   ///< Capacity of the volume that contains the root.
    LiveStatistics live;                 ///< Partial results; filled only while the state is Running.
};

/**
 * @brief Owns the scan worker thread. At most one scan runs at a time.
 * @details A finished scan is turned into a Snapshot and published to the bridge in a
 * single step; a cancelled or failed scan leaves the previous snapshot intact.
 * All public methods are thread-safe.
 */
class ScanController final {
public:
    /**
     * @brief Create an idle controller.
     * @param bridge   Receives every completed snapshot.
     * @param options  Scanner tunables.
     * @param executor Optional external executor used for directory tasks
     *                 (must outlive the controller and offer >= 1 worker that
     *                 is not used to run the controller itself).
     */
    explicit ScanController(SnapshotBridge& bridge, ScanOptions options = {}, Executor* executor = nullptr);
    ~ScanController();
    ScanController(const ScanController&) = delete;
    ScanController& operator=(const ScanController&) = delete;

    /**
     * @brief Start scanning @p root on the worker thread.
     * @param root Directory to scan.
     * @return Nothing on success, or a localized message (already running, invalid path...).
     */
    [[nodiscard]] std::expected<void, std::string> start(const std::filesystem::path& root);
    /** @brief Ask the running scan to stop; returns immediately. */
    void cancel();
    /** @brief Consistent copy of the current status; safe from any thread. */
    [[nodiscard]] ScanStatus status() const;
    /** @brief Block until no scan is running. */
    void wait();

private:
    void run(std::filesystem::path root, std::stop_token token);

    SnapshotBridge& bridge_;
    ScanOptions options_;
    Executor* executor_;

    std::mutex lifecycleMutex_; ///< Serialises start()/cancel()/destruction of worker_.
    mutable std::mutex mutex_;  ///< Guards status_ and started_.
    std::condition_variable idle_;
    ScanStatus status_;
    std::chrono::steady_clock::time_point started_;
    std::jthread worker_;       ///< Declared last: joined explicitly in the destructor.
};

} // namespace diskutilities::web
