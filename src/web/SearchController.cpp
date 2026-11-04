/**
 * @file SearchController.cpp
 * @brief SearchController implementation.
 * @details The worker thread runs the FileSearcher and appends every batch of matches under the controller mutex;
 *          readers therefore always see a consistent status together with the matches found so far.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/web/SearchController.h"

#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <numeric>
#include <system_error>
#include <utility>

namespace diskutilities::web {

SearchSort parseSearchSort(const std::string& text) noexcept {
    if (text == "name") return SearchSort::Name;
    if (text == "size") return SearchSort::Size;
    if (text == "modified") return SearchSort::Modified;
    if (text == "created") return SearchSort::Created;
    return SearchSort::Path;
}

SearchController::SearchController(Executor* executor) : executor_(executor) {}

SearchController::~SearchController() {
    std::lock_guard lifecycle(lifecycleMutex_);
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

std::expected<void, std::string> SearchController::start(const std::filesystem::path& requestedRoot,
                                                         SearchCriteria criteria, SearchOptions options) {
    std::lock_guard lifecycle(lifecycleMutex_);
    {
        std::lock_guard lock(mutex_);
        if (status_.state == ScanState::Running) return std::unexpected("È già in corso una ricerca.");
    }
    if (auto invalid = FileSearcher::validate(criteria)) return std::unexpected(invalid->message);

    std::error_code ec;
    auto root = std::filesystem::absolute(requestedRoot, ec);
    if (ec) return std::unexpected("Impossibile risolvere il percorso: " + ec.message());
    root = root.lexically_normal();

    if (worker_.joinable()) worker_.join(); // already finished: only reaps the thread

    {
        std::lock_guard lock(mutex_);
        status_ = {};
        status_.state = ScanState::Running;
        status_.root = pathToUtf8(root);
        hits_.clear();
        hits_.shrink_to_fit();
        started_ = std::chrono::steady_clock::now();
    }
    try {
        worker_ = std::jthread([this, root, criteria = std::move(criteria), options](std::stop_token token) mutable {
            run(root, std::move(criteria), options, std::move(token));
        });
    } catch (const std::system_error& ex) {
        std::lock_guard lock(mutex_);
        status_.state = ScanState::Failed;
        status_.error = std::string("Impossibile avviare il thread di ricerca: ") + ex.what();
        idle_.notify_all();
        return std::unexpected(status_.error);
    }
    return {};
}

void SearchController::cancel() {
    std::lock_guard lifecycle(lifecycleMutex_);
    worker_.request_stop();
}

SearchStatus SearchController::status() const {
    std::lock_guard lock(mutex_);
    SearchStatus copy = status_;
    if (copy.state == ScanState::Running)
        copy.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count());
    return copy;
}

SearchPage SearchController::results(std::size_t offset, std::size_t limit, SearchSort sort, bool descending) const {
    std::lock_guard lock(mutex_);
    SearchPage page;
    page.total = hits_.size();
    if (offset >= hits_.size() || limit == 0) return page;
    const std::size_t end = std::min(hits_.size(), offset + std::min<std::size_t>(limit, 100000));

    const auto key = [&](const SearchHit& h) -> std::int64_t {
        switch (sort) {
            case SearchSort::Size: return static_cast<std::int64_t>(std::min<std::uint64_t>(h.size, INT64_MAX));
            case SearchSort::Modified: return h.modified;
            case SearchSort::Created: return h.created;
            default: return 0;
        }
    };
    const auto less = [&](std::size_t a, std::size_t b) {
        const SearchHit& x = hits_[a];
        const SearchHit& y = hits_[b];
        int cmp = 0;
        if (sort == SearchSort::Name) cmp = x.name.compare(y.name);
        else if (sort != SearchSort::Path) cmp = key(x) < key(y) ? -1 : key(x) > key(y) ? 1 : 0;
        if (cmp == 0) cmp = x.path.compare(y.path);
        return descending ? cmp > 0 : cmp < 0;
    };
    std::vector<std::size_t> order(hits_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(end), order.end(), less);
    page.hits.reserve(end - offset);
    for (std::size_t i = offset; i < end; ++i) page.hits.push_back(hits_[order[i]]);
    return page;
}

void SearchController::wait() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return status_.state != ScanState::Running; });
}

void SearchController::run(std::filesystem::path root, SearchCriteria criteria, SearchOptions options,
                           std::stop_token token) {
    const auto finish = [this](ScanState state, std::string error) {
        std::lock_guard lock(mutex_);
        status_.state = state;
        status_.currentPath.clear();
        status_.error = std::move(error);
        status_.matches = hits_.size();
        status_.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count());
        idle_.notify_all();
    };
    try {
        FileSearcher searcher;
        auto result = searcher.search(
            root, criteria, options,
            [this](const SearchProgress& p) {
                std::lock_guard lock(mutex_);
                status_.examined = p.examined;
                status_.directories = p.directories;
                status_.inaccessible = p.inaccessible;
                status_.matches = p.matches;
                status_.currentPath = p.currentPath;
            },
            [this](std::span<const SearchHit> batch) {
                std::lock_guard lock(mutex_);
                hits_.insert(hits_.end(), batch.begin(), batch.end());
            },
            token, executor_);
        if (!result) {
            finish(result.error().code == SearchError::Code::Cancelled ? ScanState::Cancelled : ScanState::Failed,
                   result.error().message);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            status_.examined = result->examined;
            status_.directories = result->directories;
            status_.inaccessible = result->inaccessible;
            status_.truncated = result->truncated;
            status_.creationUnavailable = result->creationUnavailable;
        }
        finish(ScanState::Completed, {});
    } catch (const std::exception& ex) {
        finish(ScanState::Failed, ex.what());
    } catch (...) {
        finish(ScanState::Failed, "Errore sconosciuto durante la ricerca.");
    }
}

} // namespace diskutilities::web
