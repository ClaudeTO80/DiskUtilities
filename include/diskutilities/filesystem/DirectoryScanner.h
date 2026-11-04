#pragma once

/**
 * @file DirectoryScanner.h
 * @brief Read-only, parallel directory tree scanner.
 * @details Every directory is one task posted to an Executor. The scanner records symbolic links but never
 *          follows them, reports failures through std::expected and honours cooperative cancellation through a
 *          std::stop_token.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/core/Executor.h"
#include "diskutilities/filesystem/FileTree.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace diskutilities {

/** @brief Number of extensions reported by LiveStatistics (the biggest by total size). */
inline constexpr std::size_t kLiveExtensions = 50;
/** @brief Number of top-level directories reported by LiveStatistics (the biggest by total size). */
inline constexpr std::size_t kLiveDirectories = 20;

/** @brief Running total for one file extension while a scan is in progress. */
struct LiveExtension {
    std::string extension;     ///< Lower-cased extension with the dot; empty for files without one.
    std::uint64_t count = 0;   ///< Regular files seen so far with this extension.
    std::uint64_t bytes = 0;   ///< Their combined size in bytes.
};

/** @brief Running total for one direct child directory of the scan root. */
struct LiveDirectory {
    std::string name;          ///< Directory name (UTF-8).
    std::string path;          ///< Absolute path (UTF-8, '/' separators).
    std::uint64_t bytes = 0;   ///< Bytes of the regular files found so far below it.
};

/** @brief One of the biggest files seen so far. */
struct LiveFile {
    std::string name;          ///< File name (UTF-8).
    std::string path;          ///< Absolute path (UTF-8, '/' separators).
    std::string extension;     ///< Lower-cased extension with the dot; empty if none.
    std::uint64_t size = 0;    ///< Size in bytes.
    std::int64_t modified = kNoTimestamp; ///< Seconds since the Unix epoch, or kNoTimestamp.
};

/**
 * @brief Partial results gathered while the scan is still running.
 * @details Maintained incrementally by the workers (one merge per directory), so producing it costs
 * O(extensions + directories + files kept) per progress callback, never O(entries). Once the scan completes
 * the values equal those of the final Snapshot, except for the relative order of files of identical size.
 */
struct LiveStatistics {
    std::uint64_t extensionCount = 0;        ///< Number of distinct extensions seen so far.
    std::vector<LiveExtension> extensions;   ///< Up to kLiveExtensions, total size descending.
    std::vector<LiveDirectory> directories;  ///< Up to kLiveDirectories, size descending.
    std::vector<LiveFile> largestFiles;      ///< Up to ScanOptions::liveLargestFiles, size descending.
};

/** @brief Snapshot of the scan counters, delivered to the progress callback. */
struct ScanProgress {
    std::uint64_t entriesVisited = 0;   ///< Entries examined so far.
    std::uint64_t filesFound = 0;       ///< Regular files found so far.
    std::uint64_t directoriesFound = 0; ///< Directories found so far, root included.
    std::uint64_t inaccessibleFound = 0; ///< Entries or directories that could not be read.
    std::uint64_t bytesFound = 0;       ///< Combined size of the regular files found so far.
    LiveStatistics live;                ///< Partial results; empty when ScanOptions::liveStatistics is false.
    std::string currentPath; ///< A directory being read right now (UTF-8, best effort).
};

/** @brief Failure reported by DirectoryScanner::scan(). */
struct ScanError {
    /** @brief Machine-readable failure category. */
    enum class Code {
        InvalidPath,   ///< Root cannot be resolved or accessed.
        NotADirectory, ///< Root exists but is not a directory.
        Cancelled,     ///< The stop token was triggered.
        TooManyEntries,///< More entries than ScanOptions::maxNodes or the 32-bit node index allow.
        Internal       ///< Unexpected failure (allocation, executor, ...).
    };
    Code code = Code::Internal; ///< Failure category.
    std::string message; ///< Human readable, Italian (shown by the UI).
};

/** @brief Tunables for DirectoryScanner::scan(). */
struct ScanOptions {
    /** Worker threads used when no external Executor is supplied; 0 = automatic. */
    unsigned threads = 0;
    /**
     * Do not descend into directories living on a different filesystem than the
     * root (mount points). Honoured on POSIX only; ignored elsewhere.
     */
    bool stayOnFileSystem = false;
    /**
     * Abort with ScanError::Code::TooManyEntries once this many entries have been
     * collected (0 = no limit beyond the 32-bit index). Lets callers bound memory use.
     */
    std::uint64_t maxNodes = 0;
    /**
     * Collect LiveStatistics for the progress callback. Costs one small merge per directory; disable it
     * for the lowest possible overhead when nobody displays partial results.
     */
    bool liveStatistics = true;
    /** Number of biggest files kept in LiveStatistics::largestFiles (0 = keep none). */
    std::size_t liveLargestFiles = 100;
    /** Minimum delay between two progress callbacks. */
    std::chrono::milliseconds progressInterval{100};
};

/**
 * @brief Read-only filesystem scanner. Symbolic links are recorded but never followed.
 * @details The scan is parallel: every directory is one task posted to an Executor.
 * Progress callbacks are always invoked from the thread that called scan(),
 * never from a worker, so the callback needs no extra synchronisation with
 * respect to scan() itself.
 */
class DirectoryScanner final {
public:
    /** @brief Observer invoked periodically with the current counters. */
    using ProgressCallback = std::function<void(const ScanProgress&)>;

    /**
     * @brief Scan a directory tree.
     * @param root      Directory to scan (relative paths are made absolute).
     * @param options   Scan tunables.
     * @param progress  Optional progress observer (called roughly every options.progressInterval).
     * @param stopToken Cooperative cancellation; yields ScanError::Code::Cancelled.
     * @param executor  Optional external executor; when null a private StdThreadPool is used.
     * @return The scanned tree, or the reason why the scan could not complete.
     */
    [[nodiscard]] std::expected<FileTree, ScanError> scan(
        const std::filesystem::path& root,
        const ScanOptions& options = {},
        ProgressCallback progress = {},
        std::stop_token stopToken = {},
        Executor* executor = nullptr) const;
};

} // namespace diskutilities
