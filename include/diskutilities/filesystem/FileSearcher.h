#pragma once

/**
 * @file FileSearcher.h
 * @brief Read-only, parallel, cancellable file search with name, size and date filters.
 * @details Searches the live filesystem (no prior scan needed). Like the DirectoryScanner every directory is
 *          one task posted to an Executor, symbolic links are never followed, and failures are reported with
 *          std::expected. Name filters are evaluated on the name alone, so entries that cannot match cost no
 *          metadata call; size and date filters trigger exactly one lstat() per candidate (plus one statx()
 *          when a creation-time filter is used on Linux).
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
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace diskutilities {

/** @brief Which kinds of entries can be reported. */
enum class SearchKind : std::uint8_t {
    Files,       ///< Regular files only.
    Directories, ///< Directories only (size filters then reject everything: directories have no size).
    Any          ///< Regular files and directories. Symbolic links and special files are never reported.
};

/**
 * @brief What to look for. Every field is optional; all given conditions must hold (logical AND).
 * @details Timestamps are seconds since the Unix epoch and bounds are inclusive. An entry whose timestamp is
 *          unknown never satisfies a filter on that timestamp.
 */
struct SearchCriteria {
    std::string nameContains;                ///< Substring of the file name; empty = no condition.
    std::vector<std::string> patterns;       ///< Wildcards (see Glob.h); the name must match at least one.
    std::vector<std::string> excludePatterns;///< Wildcards; the name must match none of them.
    bool caseSensitive = false;              ///< Applies to nameContains and both pattern lists (ASCII folding).
    SearchKind kind = SearchKind::Files;     ///< Kinds of entries to report.
    bool includeHidden = true;               ///< False skips names starting with '.' and does not enter them.
    std::optional<std::uint64_t> minSize;    ///< Smallest size in bytes (files only).
    std::optional<std::uint64_t> maxSize;    ///< Largest size in bytes (files only).
    std::optional<std::int64_t> modifiedAfter;   ///< Earliest modification time.
    std::optional<std::int64_t> modifiedBefore;  ///< Latest modification time.
    std::optional<std::int64_t> createdAfter;    ///< Earliest creation time (needs platform support).
    std::optional<std::int64_t> createdBefore;   ///< Latest creation time (needs platform support).

    /** @brief True if a creation-time bound is set. */
    [[nodiscard]] bool usesCreation() const noexcept { return createdAfter || createdBefore; }
};

/** @brief One matching entry. */
struct SearchHit {
    std::string path;                        ///< Absolute path (UTF-8, '/' separators).
    std::string name;                        ///< File name (UTF-8).
    EntryType type = EntryType::RegularFile; ///< RegularFile or Directory.
    std::uint64_t size = 0;                  ///< Bytes (files only, 0 for directories).
    std::int64_t modified = kNoTimestamp;    ///< Modification time, or kNoTimestamp.
    std::int64_t created = kNoTimestamp;     ///< Creation time; filled when a creation filter is set or SearchOptions::reportCreation.
};

/** @brief Counters delivered to the progress callback. */
struct SearchProgress {
    std::uint64_t examined = 0;     ///< Entries looked at so far.
    std::uint64_t directories = 0;  ///< Directories entered so far, root included.
    std::uint64_t inaccessible = 0; ///< Directories or entries that could not be read.
    std::uint64_t matches = 0;      ///< Entries that satisfied every condition so far.
    std::string currentPath;        ///< A directory being read right now (UTF-8, best effort).
};

/** @brief Final outcome of a search. */
struct SearchResult {
    std::vector<SearchHit> hits;     ///< Matches sorted by path (at most SearchOptions::maxResults).
    std::uint64_t examined = 0;      ///< Entries looked at.
    std::uint64_t directories = 0;   ///< Directories entered, root included.
    std::uint64_t inaccessible = 0;  ///< Directories or entries that could not be read.
    bool truncated = false;          ///< The result cap was reached and the search stopped early; the hits
                                     ///< are then an arbitrary subset of all matches.
    std::uint64_t creationUnavailable = 0; ///< Candidates rejected only because the creation time is unknown.
};

/** @brief Failure reported by FileSearcher::search(). */
struct SearchError {
    /** @brief Machine-readable failure category. */
    enum class Code {
        InvalidPath,     ///< Root cannot be resolved or accessed.
        NotADirectory,   ///< Root exists but is not a directory.
        InvalidCriteria, ///< Contradictory or malformed criteria (message says which).
        Cancelled,       ///< The stop token was triggered.
        Internal         ///< Unexpected failure (allocation, executor, ...).
    };
    Code code = Code::Internal; ///< Failure category.
    std::string message;        ///< Human readable, Italian (shown by the UI).
};

/** @brief Tunables for FileSearcher::search(). */
struct SearchOptions {
    /** Worker threads used when no external Executor is supplied; 0 = automatic. */
    unsigned threads = 0;
    /** Stop after this many matches (0 = use the default of 100000). Bounds memory and result size. */
    std::uint64_t maxResults = 10000;
    /** Do not descend into directories on a different filesystem than the root (POSIX only). */
    bool stayOnFileSystem = false;
    /**
     * Fill SearchHit::created for every match even when no creation filter is set (one extra statx() per match
     * on Linux). Off by default; the GUI turns it on to show the column.
     */
    bool reportCreation = false;
    /** Minimum delay between two progress callbacks. */
    std::chrono::milliseconds progressInterval{100};
};

/**
 * @brief Parallel file search over the live filesystem.
 * @details Progress callbacks run on the thread that called search(). The hits callback runs on worker threads
 * but is serialised (never called concurrently) and receives one batch per directory that contained matches;
 * it must return quickly.
 */
class FileSearcher final {
public:
    /** @brief Observer invoked periodically with the current counters. */
    using ProgressCallback = std::function<void(const SearchProgress&)>;
    /** @brief Observer receiving every new batch of matches as soon as it is found. */
    using HitsCallback = std::function<void(std::span<const SearchHit>)>;

    /**
     * @brief Check @p criteria without running a search.
     * @return std::nullopt when valid, otherwise the reason (code InvalidCriteria).
     */
    [[nodiscard]] static std::optional<SearchError> validate(const SearchCriteria& criteria);

    /**
     * @brief Search @p root recursively.
     * @param root      Directory to search (relative paths are made absolute).
     * @param criteria  Conditions every reported entry must satisfy.
     * @param options   Tunables.
     * @param progress  Optional progress observer.
     * @param onHits    Optional live observer of the matches.
     * @param stopToken Cooperative cancellation; yields SearchError::Code::Cancelled.
     * @param executor  Optional external executor; when null a private StdThreadPool is used.
     * @return The matches and counters, or the reason the search could not run.
     */
    [[nodiscard]] std::expected<SearchResult, SearchError> search(
        const std::filesystem::path& root,
        const SearchCriteria& criteria,
        const SearchOptions& options = {},
        ProgressCallback progress = {},
        HitsCallback onHits = {},
        std::stop_token stopToken = {},
        Executor* executor = nullptr) const;
};

} // namespace diskutilities
