#pragma once

/**
 * @file SearchController.h
 * @brief Runs file searches on a background thread and exposes live, sortable, paged results.
 * @details At most one search runs at a time. The UI polls status() and results(); matches become visible as
 *          soon as the searcher finds them. A new search replaces the results of the previous one when it starts.
 *          Independent from the ScanController: a search and a disk scan may run at the same time.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/core/Executor.h"
#include "diskutilities/filesystem/FileSearcher.h"
#include "diskutilities/web/ScanController.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace diskutilities::web {

/** @brief Observable state of a search; a plain value, safe to copy across threads. */
struct SearchStatus {
    ScanState state = ScanState::Idle;  ///< Lifecycle (shared vocabulary with scans).
    std::string root;                   ///< UTF-8 root of the current or last search.
    std::string currentPath;            ///< Directory being read right now (best effort).
    std::string error;                  ///< Failure description when the state is Failed.
    std::uint64_t examined = 0;         ///< Entries looked at so far.
    std::uint64_t directories = 0;      ///< Directories entered so far.
    std::uint64_t inaccessible = 0;     ///< Directories or entries that could not be read.
    std::uint64_t matches = 0;          ///< Matches found so far (also while running).
    std::uint64_t elapsedMs = 0;        ///< Milliseconds since the search started.
    bool truncated = false;             ///< The result cap was reached.
    std::uint64_t creationUnavailable = 0; ///< Candidates skipped because their creation time is unknown.
};

/** @brief Column used to order SearchController::results(). */
enum class SearchSort { Path, Name, Size, Modified, Created };

/** @brief Parse "path", "name", "size", "modified" or "created"; unknown text yields Path. */
[[nodiscard]] SearchSort parseSearchSort(const std::string& text) noexcept;

/** @brief One page of matches. */
struct SearchPage {
    std::uint64_t total = 0;          ///< Matches currently stored.
    std::vector<SearchHit> hits;      ///< The requested slice after sorting.
};

/**
 * @brief Owns the search worker thread. All public methods are thread-safe.
 */
class SearchController final {
public:
    /**
     * @param executor Optional external executor for the directory tasks (must outlive the controller).
     */
    explicit SearchController(Executor* executor = nullptr);
    ~SearchController();
    SearchController(const SearchController&) = delete;
    SearchController& operator=(const SearchController&) = delete;

    /**
     * @brief Start a search; the previous results are discarded.
     * @return Nothing on success, or a localized message (already running, invalid criteria or path...).
     */
    [[nodiscard]] std::expected<void, std::string> start(const std::filesystem::path& root,
                                                         SearchCriteria criteria, SearchOptions options = {});
    /** @brief Ask the running search to stop; returns immediately. Matches found so far stay available. */
    void cancel();
    /** @brief Consistent copy of the current status. */
    [[nodiscard]] SearchStatus status() const;
    /**
     * @brief A slice of the matches ordered by @p sort.
     * @param offset     Index of the first hit to return.
     * @param limit      Maximum number of hits to return.
     * @param sort       Ordering column (ties broken by path).
     * @param descending Reverse the order.
     */
    [[nodiscard]] SearchPage results(std::size_t offset, std::size_t limit, SearchSort sort = SearchSort::Path,
                                     bool descending = false) const;
    /** @brief Block until no search is running. */
    void wait();

private:
    void run(std::filesystem::path root, SearchCriteria criteria, SearchOptions options, std::stop_token token);

    Executor* executor_;
    std::mutex lifecycleMutex_;     ///< Serialises start()/cancel()/destruction of worker_.
    mutable std::mutex mutex_;      ///< Guards status_, hits_ and started_.
    std::condition_variable idle_;
    SearchStatus status_;
    std::vector<SearchHit> hits_;
    std::chrono::steady_clock::time_point started_;
    std::jthread worker_;           ///< Declared last: joined explicitly in the destructor.
};

} // namespace diskutilities::web
