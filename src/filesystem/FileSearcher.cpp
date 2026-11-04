/**
 * @file FileSearcher.cpp
 * @brief Parallel file search implementation.
 * @details One task per directory, same scheme as the DirectoryScanner. Name conditions are checked before any
 *          metadata call; matches of one directory are published in a single batch.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FileSearcher.h"

#include "diskutilities/core/TaskGroup.h"
#include "diskutilities/filesystem/EntryStat.h"
#include "diskutilities/filesystem/Glob.h"
#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>

namespace diskutilities {

namespace {

constexpr unsigned kMaxAutoThreads = 4;
constexpr std::uint64_t kDefaultMaxResults = 100000;

/** State shared by the waiting thread and every directory task of one search. */
struct Context {
    const SearchCriteria* criteria = nullptr;
    bool withCreation = false;
    bool useDevice = false;
    std::uint64_t rootDevice = 0;
    std::uint64_t maxResults = kDefaultMaxResults;

    std::atomic<std::uint64_t> examined{0};
    std::atomic<std::uint64_t> directories{1}; // the root itself
    std::atomic<std::uint64_t> inaccessible{0};
    std::atomic<std::uint64_t> matches{0};
    std::atomic<std::uint64_t> creationUnavailable{0};
    std::atomic<bool> truncated{false};
    std::atomic<bool> aborted{false};

    std::mutex hitsMutex; // guards hits and serialises the callback
    std::vector<SearchHit> hits;
    FileSearcher::HitsCallback onHits;

    std::mutex pathMutex;
    std::string currentPath;

    std::mutex errorMutex;
    std::optional<SearchError> error;

    TaskGroup tasks;
    std::stop_token stop;
    Executor* executor = nullptr;

    void fail(SearchError::Code code, std::string message) {
        {
            std::lock_guard lock(errorMutex);
            if (!error) error = SearchError{code, std::move(message)};
        }
        aborted.store(true, std::memory_order_relaxed);
    }

    bool shouldStop() {
        if (aborted.load(std::memory_order_relaxed)) return true;
        if (stop.stop_requested()) {
            aborted.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
};

bool nameMatches(const SearchCriteria& c, std::string_view name) {
    if (!containsText(name, c.nameContains, c.caseSensitive)) return false;
    if (!c.patterns.empty()) {
        const bool any = std::any_of(c.patterns.begin(), c.patterns.end(),
                                     [&](const std::string& p) { return globMatch(p, name, c.caseSensitive); });
        if (!any) return false;
    }
    return std::none_of(c.excludePatterns.begin(), c.excludePatterns.end(),
                        [&](const std::string& p) { return globMatch(p, name, c.caseSensitive); });
}

bool inRange(std::int64_t value, const std::optional<std::int64_t>& after, const std::optional<std::int64_t>& before) {
    if (!after && !before) return true;
    if (value == kNoTimestamp) return false;
    return (!after || value >= *after) && (!before || value <= *before);
}

void spawn(Context& ctx, std::filesystem::path directory);

void flush(Context& ctx, std::vector<SearchHit>& batch) {
    if (batch.empty()) return;
    std::lock_guard lock(ctx.hitsMutex);
    if (ctx.onHits) ctx.onHits(std::span<const SearchHit>(batch));
    for (auto& hit : batch) ctx.hits.push_back(std::move(hit));
    batch.clear();
}

void readDirectory(Context& ctx, const std::filesystem::path& directory) {
    if (ctx.shouldStop()) return;
    const SearchCriteria& c = *ctx.criteria;

    {
        std::lock_guard lock(ctx.pathMutex);
        ctx.currentPath = pathToUtf8(directory);
    }

    std::error_code ec;
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, ec);
    if (ec) {
        ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::vector<SearchHit> batch;
    std::vector<std::filesystem::path> subdirectories;
    std::string directoryUtf8;
    const std::filesystem::directory_iterator end;

    while (it != end) {
        if (ctx.shouldStop()) break;
        const std::filesystem::directory_entry& entry = *it;
        ctx.examined.fetch_add(1, std::memory_order_relaxed);

        std::error_code typeEc;
        const auto status = entry.symlink_status(typeEc); // d_type is cached by the iterator: usually no syscall
        const bool isDirectory = !typeEc && std::filesystem::is_directory(status);
        const bool isFile = !typeEc && std::filesystem::is_regular_file(status);
        if (typeEc) ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);

        // Matching runs on the raw file name without allocating; the UTF-8 repaired copy is built only for hits.
        // (So a name with an ill-formed byte is matched by its original bytes, '?' consuming one byte per bad byte.)
#if defined(_WIN32)
        const std::string nativeName = pathToUtf8(entry.path().filename());
        const std::string_view nameView = nativeName;
#else
        const std::string& nativePath = entry.path().native();
        const std::size_t slash = nativePath.rfind('/');
        const std::string_view nameView = std::string_view(nativePath).substr(slash == std::string::npos ? 0 : slash + 1);
#endif
        const bool hidden = !nameView.empty() && nameView.front() == '.';
        if (!(hidden && !c.includeHidden) && (isDirectory || isFile)) {
            if (isDirectory) {
                bool descend = true;
                if (ctx.useDevice) {
                    const detail::EntryStat st = detail::probeEntry(entry);
                    descend = st.ok && st.device == ctx.rootDevice;
                }
                if (descend) {
                    ctx.directories.fetch_add(1, std::memory_order_relaxed);
                    subdirectories.push_back(entry.path());
                }
            }

            const bool kindWanted = isFile ? c.kind != SearchKind::Directories : c.kind != SearchKind::Files;
            // Size conditions can never hold for a directory.
            const bool sizeRejectsDir = isDirectory && (c.minSize || c.maxSize);
            if (kindWanted && !sizeRejectsDir && nameMatches(c, nameView)) {
                detail::EntryStat st;
                bool pass = true;
                st = detail::probeEntry(entry, ctx.withCreation); // matches need size/dates for display anyway
                if (!st.ok) {
                    ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);
                    pass = false;
                }
                if (pass && isFile) {
                    if (c.minSize && st.size < *c.minSize) pass = false;
                    if (c.maxSize && st.size > *c.maxSize) pass = false;
                }
                if (pass && !inRange(st.modified, c.modifiedAfter, c.modifiedBefore)) pass = false;
                if (pass && c.usesCreation() && !inRange(st.created, c.createdAfter, c.createdBefore)) {
                    if (st.created == kNoTimestamp) ctx.creationUnavailable.fetch_add(1, std::memory_order_relaxed);
                    pass = false;
                }
                if (pass) {
                    if (ctx.matches.fetch_add(1, std::memory_order_relaxed) >= ctx.maxResults) {
                        ctx.matches.fetch_sub(1, std::memory_order_relaxed);
                        ctx.truncated.store(true, std::memory_order_relaxed);
                        ctx.aborted.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (directoryUtf8.empty()) directoryUtf8 = toValidUtf8(pathToUtf8(directory));
                    SearchHit hit;
                    std::string name = toValidUtf8(std::string(nameView));
                    hit.path = directoryUtf8;
                    if (hit.path.empty() || hit.path.back() != '/') hit.path += '/';
                    hit.path += name;
                    hit.name = std::move(name);
                    hit.type = isDirectory ? EntryType::Directory : EntryType::RegularFile;
                    hit.size = isFile ? st.size : 0;
                    hit.modified = st.modified;
                    hit.created = st.created;
                    batch.push_back(std::move(hit));
                }
            }
        }

        ec.clear();
        it.increment(ec);
        if (ec) {
            ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    flush(ctx, batch);
    if (!ctx.aborted.load(std::memory_order_relaxed))
        for (auto& sub : subdirectories) spawn(ctx, std::move(sub));
}

void spawn(Context& ctx, std::filesystem::path directory) {
    ctx.tasks.add();
    try {
        ctx.executor->post([&ctx, directory = std::move(directory)] {
            try {
                readDirectory(ctx, directory);
            } catch (const std::bad_alloc&) {
                ctx.fail(SearchError::Code::Internal, "Memoria insufficiente durante la ricerca.");
            } catch (const std::exception& ex) {
                ctx.fail(SearchError::Code::Internal, ex.what());
            } catch (...) {
                ctx.fail(SearchError::Code::Internal, "Errore sconosciuto durante la ricerca.");
            }
            ctx.tasks.done();
        });
    } catch (const std::exception& ex) {
        ctx.fail(SearchError::Code::Internal, std::string("Impossibile pianificare la ricerca: ") + ex.what());
        ctx.tasks.done();
    }
}

SearchProgress snapshotProgress(Context& ctx) {
    SearchProgress p;
    p.examined = ctx.examined.load(std::memory_order_relaxed);
    p.directories = ctx.directories.load(std::memory_order_relaxed);
    p.inaccessible = ctx.inaccessible.load(std::memory_order_relaxed);
    p.matches = ctx.matches.load(std::memory_order_relaxed);
    std::lock_guard lock(ctx.pathMutex);
    p.currentPath = ctx.currentPath;
    return p;
}

} // namespace

std::optional<SearchError> FileSearcher::validate(const SearchCriteria& c) {
    const auto bad = [](std::string message) {
        return std::optional<SearchError>(SearchError{SearchError::Code::InvalidCriteria, std::move(message)});
    };
    if (c.minSize && c.maxSize && *c.minSize > *c.maxSize)
        return bad("La dimensione minima supera quella massima.");
    if (c.modifiedAfter && c.modifiedBefore && *c.modifiedAfter > *c.modifiedBefore)
        return bad("Intervallo delle date di modifica invertito.");
    if (c.createdAfter && c.createdBefore && *c.createdAfter > *c.createdBefore)
        return bad("Intervallo delle date di creazione invertito.");
    const auto empty = [](const std::string& s) { return s.empty(); };
    if (std::any_of(c.patterns.begin(), c.patterns.end(), empty) ||
        std::any_of(c.excludePatterns.begin(), c.excludePatterns.end(), empty))
        return bad("Un pattern è vuoto.");
    return std::nullopt;
}

std::expected<SearchResult, SearchError> FileSearcher::search(const std::filesystem::path& requestedRoot,
                                                              const SearchCriteria& criteria,
                                                              const SearchOptions& options,
                                                              ProgressCallback progress,
                                                              HitsCallback onHits,
                                                              std::stop_token stopToken,
                                                              Executor* executor) const {
    if (auto invalid = validate(criteria)) return std::unexpected(*invalid);

    std::error_code ec;
    auto root = std::filesystem::absolute(requestedRoot, ec);
    if (ec) return std::unexpected(SearchError{SearchError::Code::InvalidPath,
                                               "Impossibile risolvere il percorso: " + ec.message()});
    root = root.lexically_normal();
    const auto rootStatus = std::filesystem::status(root, ec);
    if (ec) return std::unexpected(SearchError{SearchError::Code::InvalidPath,
                                               "Impossibile accedere al percorso: " + ec.message()});
    if (!std::filesystem::is_directory(rootStatus))
        return std::unexpected(SearchError{SearchError::Code::NotADirectory, "Il percorso non è una directory."});
    if (stopToken.stop_requested())
        return std::unexpected(SearchError{SearchError::Code::Cancelled, "Ricerca annullata dall'utente."});

    Context ctx;
    ctx.criteria = &criteria;
    ctx.withCreation = criteria.usesCreation() || options.reportCreation;
    ctx.stop = stopToken;
    ctx.onHits = std::move(onHits);
    ctx.maxResults = options.maxResults != 0 ? options.maxResults : kDefaultMaxResults;

    const detail::EntryStat rootStat = detail::probeEntry(std::filesystem::directory_entry(root / "."));
    ctx.useDevice = options.stayOnFileSystem && rootStat.ok && rootStat.device != 0;
    ctx.rootDevice = rootStat.device;

    std::optional<StdThreadPool> ownedPool;
    if (executor == nullptr) {
        unsigned threads = options.threads;
        if (threads == 0) threads = std::clamp(std::thread::hardware_concurrency(), 1u, kMaxAutoThreads);
        try {
            ownedPool.emplace(threads);
        } catch (const std::exception& ex) {
            return std::unexpected(SearchError{SearchError::Code::Internal,
                                               std::string("Impossibile creare i thread di ricerca: ") + ex.what()});
        }
        executor = &*ownedPool;
    }
    ctx.executor = executor;

    const auto interval = std::max(options.progressInterval, std::chrono::milliseconds(1));
    spawn(ctx, root);
    while (!ctx.tasks.waitFor(interval))
        if (progress) progress(snapshotProgress(ctx));

    if (ctx.error) return std::unexpected(*ctx.error);
    if (ctx.aborted.load() && !ctx.truncated.load())
        return std::unexpected(SearchError{SearchError::Code::Cancelled, "Ricerca annullata dall'utente."});

    if (progress) {
        auto last = snapshotProgress(ctx);
        last.currentPath.clear();
        progress(last);
    }

    SearchResult result;
    result.hits = std::move(ctx.hits);
    std::sort(result.hits.begin(), result.hits.end(),
              [](const SearchHit& a, const SearchHit& b) { return a.path < b.path; });
    result.examined = ctx.examined.load();
    result.directories = ctx.directories.load();
    result.inaccessible = ctx.inaccessible.load();
    result.truncated = ctx.truncated.load();
    result.creationUnavailable = ctx.creationUnavailable.load();
    return result;
}

} // namespace diskutilities
