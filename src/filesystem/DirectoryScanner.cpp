/**
 * @file DirectoryScanner.cpp
 * @brief Parallel directory scanner implementation.
 * @details One task per directory: a task lists its directory, appends the entries to the shared tree under a
 *          mutex and posts one new task per sub-directory. Completion is tracked with a pending-task counter
 *          decremented under a mutex, so the waiting thread can never destroy the shared context while a worker
 *          still uses it.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/DirectoryScanner.h"
#include "diskutilities/core/TaskGroup.h"

#include "diskutilities/filesystem/EntryStat.h"
#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace diskutilities {
namespace {

/** Upper bound on the automatic worker count: more threads rarely help on one disk. */
constexpr unsigned kMaxAutoThreads = 4;

/** Largest node count addressable by the 32-bit parent index (kNoParent is reserved). */
constexpr std::size_t kMaxNodes = static_cast<std::size_t>(kNoParent) - 1;

/** Marker for "not below any direct child directory of the root". */
constexpr std::uint32_t kNoTop = std::numeric_limits<std::uint32_t>::max();

/** Transparent hash so that the extension map can be probed with a string_view (no allocation). */
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};

/** Count and bytes of one extension. */
struct ExtensionTotal {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
};
using ExtensionMap = std::unordered_map<std::string, ExtensionTotal, StringHash, std::equal_to<>>;

/** Ranking of the biggest files: larger first, then lexicographic path (deterministic). */
bool isBetter(const LiveFile& a, const LiveFile& b) {
    if (a.size != b.size) return a.size > b.size;
    return a.path < b.path;
}

/** Join a UTF-8 directory path and a name with exactly one '/'. */
std::string joinPath(const std::string& directory, const std::string& name) {
    std::string result = directory;
    if (result.empty() || result.back() != '/') result.push_back('/');
    result += name;
    return result;
}

/**
 * Partial results shared by all workers. Each directory task accumulates locally without any lock and
 * merges once (see LiveBatch), so contention is one short critical section per directory.
 */
struct LiveState {
    bool enabled = false;
    std::size_t capacity = 0; ///< Number of biggest files kept.

    std::mutex mutex;
    ExtensionMap extensions;
    std::vector<LiveDirectory> directories; ///< Indexed by the "top" id handed to the tasks.
    std::vector<LiveFile> largest;          ///< Binary heap: the worst kept file is at the front.

    /// Size of the worst kept file once the heap is full (0 before): smaller files cannot enter.
    std::atomic<std::uint64_t> floor{0};
};

/** One directory task's contribution to the live statistics, built lock-free and merged once. */
struct LiveBatch {
    struct Candidate {
        std::uint64_t size;
        std::size_t index; ///< Position in the task's local node vector.
    };
    std::uint64_t bytes = 0;
    ExtensionMap extensions;
    std::vector<Candidate> candidates;
};

/** State shared by the waiting thread and every directory task of one scan. */
struct Context {
    // Result under construction; guarded by treeMutex.
    std::mutex treeMutex;
    FileTree tree;

    // Counters read by the progress reporter; relaxed atomics are enough.
    std::atomic<std::uint64_t> visited{0};
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> directories{1}; // the root itself
    std::atomic<std::uint64_t> inaccessible{0};
    std::atomic<std::uint64_t> bytes{0};

    LiveState live;
    std::string rootUtf8;

    std::mutex pathMutex;
    std::string currentPath;

    // Completion tracking (see TaskGroup: the waiter can never free this object while a worker uses it).
    TaskGroup tasks;

    std::mutex errorMutex;
    std::optional<ScanError> error;
    std::atomic<bool> aborted{false}; // stop requested or fatal error: workers bail out early

    std::stop_token stop;
    Executor* executor = nullptr;
    std::size_t maxNodes = kMaxNodes;
    bool useDevice = false;
    std::uint64_t rootDevice = 0;

    void fail(ScanError::Code code, std::string message) {
        {
            std::lock_guard lock(errorMutex);
            if (!error) error = ScanError{code, std::move(message)};
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

void spawn(Context& ctx, std::filesystem::path directory, std::uint32_t index, std::uint32_t top);

/** Add the batch of one directory to the shared live statistics. */
void mergeLive(Context& ctx, LiveBatch& batch, const std::string& directoryUtf8,
               const std::vector<Node>& local, std::uint32_t top) {
    LiveState& live = ctx.live;
    // Keep only this directory's best candidates: nothing else can enter the global ranking.
    if (batch.candidates.size() > live.capacity) {
        std::nth_element(batch.candidates.begin(), batch.candidates.begin() + static_cast<std::ptrdiff_t>(live.capacity),
                         batch.candidates.end(),
                         [](const auto& a, const auto& b) { return a.size > b.size; });
        batch.candidates.resize(live.capacity);
    }
    std::vector<LiveFile> files;
    files.reserve(batch.candidates.size());
    for (const auto& candidate : batch.candidates) {
        const Node& node = local[candidate.index];
        LiveFile file;
        file.name = node.name;
        file.path = joinPath(directoryUtf8, node.name);
        const auto ext = rawExtension(node.name);
        file.extension.assign(ext);
        std::transform(file.extension.begin(), file.extension.end(), file.extension.begin(), asciiLower);
        file.size = node.size;
        file.modified = node.modified;
        files.push_back(std::move(file));
    }

    std::lock_guard lock(live.mutex);
    for (auto& [key, total] : batch.extensions) {
        auto& target = live.extensions[key];
        target.count += total.count;
        target.bytes += total.bytes;
    }
    if (top != kNoTop && top < live.directories.size()) live.directories[top].bytes += batch.bytes;
    const auto worseFirst = [](const LiveFile& a, const LiveFile& b) { return isBetter(a, b); };
    for (auto& file : files) {
        if (live.largest.size() < live.capacity) {
            live.largest.push_back(std::move(file));
            std::push_heap(live.largest.begin(), live.largest.end(), worseFirst);
        } else if (isBetter(file, live.largest.front())) {
            std::pop_heap(live.largest.begin(), live.largest.end(), worseFirst);
            live.largest.back() = std::move(file);
            std::push_heap(live.largest.begin(), live.largest.end(), worseFirst);
        }
    }
    if (live.largest.size() >= live.capacity && !live.largest.empty())
        live.floor.store(live.largest.front().size, std::memory_order_relaxed);
}

/** Read one directory, publish its entries and spawn a task per sub-directory. */
void readDirectory(Context& ctx, const std::filesystem::path& directory, std::uint32_t index, std::uint32_t top) {
    if (ctx.shouldStop()) return;

    {
        std::lock_guard lock(ctx.pathMutex);
        ctx.currentPath = pathToUtf8(directory);
    }

    std::error_code ec;
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, ec);
    if (ec) {
        ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(ctx.treeMutex);
        ctx.tree.nodes[index].accessible = false;
        return;
    }

    std::vector<Node> local;
    LiveBatch batch;
    std::string extensionKey;
    const std::string directoryUtf8 = ctx.live.enabled ? pathToUtf8(directory) : std::string();
    std::vector<std::size_t> subdirectoryPositions;
    std::vector<std::filesystem::path> subdirectoryPaths;
    bool listingFailed = false;

    const std::filesystem::directory_iterator end;
    while (it != end) {
        if (ctx.shouldStop()) return;
        const std::filesystem::directory_entry& entry = *it;

        Node node;
        node.name = toValidUtf8(pathToUtf8(entry.path().filename()));
        node.parent = index;
        const detail::EntryStat stat = detail::probeEntry(entry);
        node.type = stat.type;
        node.size = stat.size;
        node.modified = stat.modified;
        node.accessible = stat.ok;

        ctx.visited.fetch_add(1, std::memory_order_relaxed);
        if (!stat.ok) ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);
        if (stat.type == EntryType::RegularFile) {
            ctx.files.fetch_add(1, std::memory_order_relaxed);
            ctx.bytes.fetch_add(stat.size, std::memory_order_relaxed);
            if (ctx.live.enabled) {
                batch.bytes += stat.size;
                const auto ext = rawExtension(node.name);
                extensionKey.assign(ext);
                std::transform(extensionKey.begin(), extensionKey.end(), extensionKey.begin(), asciiLower);
                auto& total = batch.extensions[extensionKey];
                ++total.count;
                total.bytes += stat.size;
                if (ctx.live.capacity > 0 && stat.size > 0 && stat.size >= ctx.live.floor.load(std::memory_order_relaxed))
                    batch.candidates.push_back({stat.size, local.size()});
            }
        } else if (stat.type == EntryType::Directory) {
            ctx.directories.fetch_add(1, std::memory_order_relaxed);
            const bool crossesDevice = ctx.useDevice && stat.device != ctx.rootDevice;
            if (stat.ok && !crossesDevice) {
                subdirectoryPositions.push_back(local.size());
                subdirectoryPaths.push_back(entry.path());
            }
        }
        local.push_back(std::move(node));

        ec.clear();
        it.increment(ec);
        if (ec) {
            listingFailed = true;
            ctx.inaccessible.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    // Live statistics are merged while `local` still owns the names (it is moved into the tree below).
    // Direct children of the root get a "top" id: every file found below them is attributed to it.
    std::vector<std::uint32_t> subdirectoryTops(subdirectoryPositions.size(), top);
    if (ctx.live.enabled) {
        if (index == 0) {
            std::lock_guard lock(ctx.live.mutex);
            for (std::size_t k = 0; k < subdirectoryPositions.size(); ++k) {
                const std::string& name = local[subdirectoryPositions[k]].name;
                subdirectoryTops[k] = static_cast<std::uint32_t>(ctx.live.directories.size());
                ctx.live.directories.push_back({name, joinPath(directoryUtf8, name), 0});
            }
        }
        mergeLive(ctx, batch, directoryUtf8, local, top);
    }

    std::uint32_t base = 0;
    {
        std::lock_guard lock(ctx.treeMutex);
        auto& nodes = ctx.tree.nodes;
        if (nodes.size() + local.size() > ctx.maxNodes) {
            ctx.fail(ScanError::Code::TooManyEntries,
                     ctx.maxNodes < kMaxNodes
                         ? "Troppi elementi per la memoria disponibile: limite di " + std::to_string(ctx.maxNodes) + " voci superato."
                         : "Troppi elementi: superato il limite di 4 miliardi di voci.");
            return;
        }
        base = static_cast<std::uint32_t>(nodes.size());
        for (auto& node : local) nodes.push_back(std::move(node));
        if (listingFailed) nodes[index].accessible = false;
    }

    for (std::size_t k = 0; k < subdirectoryPositions.size(); ++k)
        spawn(ctx, std::move(subdirectoryPaths[k]),
              base + static_cast<std::uint32_t>(subdirectoryPositions[k]), subdirectoryTops[k]);
}

void spawn(Context& ctx, std::filesystem::path directory, std::uint32_t index, std::uint32_t top) {
    ctx.tasks.add();
    try {
        ctx.executor->post([&ctx, directory = std::move(directory), index, top] {
            try {
                readDirectory(ctx, directory, index, top);
            } catch (const std::bad_alloc&) {
                ctx.fail(ScanError::Code::Internal, "Memoria insufficiente durante la scansione.");
            } catch (const std::exception& ex) {
                ctx.fail(ScanError::Code::Internal, ex.what());
            } catch (...) {
                ctx.fail(ScanError::Code::Internal, "Errore sconosciuto durante la scansione.");
            }
            ctx.tasks.done();
        });
    } catch (const std::exception& ex) {
        ctx.fail(ScanError::Code::Internal, std::string("Impossibile pianificare la scansione: ") + ex.what());
        ctx.tasks.done();
    }
}

/** Copy the best entries of the live statistics (partial sorts: no full copy of the extension table). */
void snapshotLive(Context& ctx, LiveStatistics& out) {
    LiveState& live = ctx.live;
    std::lock_guard lock(live.mutex);

    out.extensionCount = live.extensions.size();
    std::vector<const ExtensionMap::value_type*> extensions;
    extensions.reserve(live.extensions.size());
    for (const auto& entry : live.extensions) extensions.push_back(&entry);
    const std::size_t extensionsKept = std::min(kLiveExtensions, extensions.size());
    std::partial_sort(extensions.begin(), extensions.begin() + static_cast<std::ptrdiff_t>(extensionsKept), extensions.end(),
                      [](const auto* a, const auto* b) {
                          return a->second.bytes != b->second.bytes ? a->second.bytes > b->second.bytes : a->first < b->first;
                      });
    out.extensions.reserve(extensionsKept);
    for (std::size_t i = 0; i < extensionsKept; ++i)
        out.extensions.push_back({extensions[i]->first, extensions[i]->second.count, extensions[i]->second.bytes});

    std::vector<const LiveDirectory*> directories;
    directories.reserve(live.directories.size());
    for (const auto& directory : live.directories) directories.push_back(&directory);
    const std::size_t directoriesKept = std::min(kLiveDirectories, directories.size());
    std::partial_sort(directories.begin(), directories.begin() + static_cast<std::ptrdiff_t>(directoriesKept), directories.end(),
                      [](const auto* a, const auto* b) {
                          return a->bytes != b->bytes ? a->bytes > b->bytes : a->name < b->name;
                      });
    out.directories.reserve(directoriesKept);
    for (std::size_t i = 0; i < directoriesKept; ++i) out.directories.push_back(*directories[i]);

    out.largestFiles = live.largest;
    std::sort(out.largestFiles.begin(), out.largestFiles.end(), isBetter);
}

ScanProgress snapshotProgress(Context& ctx) {
    ScanProgress progress;
    progress.entriesVisited = ctx.visited.load(std::memory_order_relaxed);
    progress.filesFound = ctx.files.load(std::memory_order_relaxed);
    progress.directoriesFound = ctx.directories.load(std::memory_order_relaxed);
    progress.inaccessibleFound = ctx.inaccessible.load(std::memory_order_relaxed);
    progress.bytesFound = ctx.bytes.load(std::memory_order_relaxed);
    if (ctx.live.enabled) snapshotLive(ctx, progress.live);
    std::lock_guard lock(ctx.pathMutex);
    progress.currentPath = ctx.currentPath;
    return progress;
}

} // namespace

std::expected<FileTree, ScanError> DirectoryScanner::scan(const std::filesystem::path& requestedRoot,
                                                          const ScanOptions& options,
                                                          ProgressCallback progress,
                                                          std::stop_token stopToken,
                                                          Executor* executor) const {
    std::error_code ec;
    auto root = std::filesystem::absolute(requestedRoot, ec);
    if (ec) return std::unexpected(ScanError{ScanError::Code::InvalidPath,
                                             "Impossibile risolvere il percorso: " + ec.message()});
    root = root.lexically_normal();

    const auto rootStatus = std::filesystem::status(root, ec);
    if (ec) return std::unexpected(ScanError{ScanError::Code::InvalidPath,
                                             "Impossibile accedere al percorso: " + ec.message()});
    if (!std::filesystem::is_directory(rootStatus))
        return std::unexpected(ScanError{ScanError::Code::NotADirectory, "Il percorso non è una directory."});
    if (stopToken.stop_requested())
        return std::unexpected(ScanError{ScanError::Code::Cancelled, "Scansione annullata dall'utente."});

    Context ctx;
    ctx.stop = stopToken;
    ctx.tree.root = root;
    ctx.rootUtf8 = pathToUtf8(root);
    ctx.live.enabled = options.liveStatistics;
    ctx.live.capacity = options.liveLargestFiles;
    if (options.maxNodes != 0) ctx.maxNodes = static_cast<std::size_t>(std::min<std::uint64_t>(options.maxNodes, kMaxNodes));

    Node rootNode;
    rootNode.name = toValidUtf8(pathToUtf8(root.filename()));
    if (rootNode.name.empty()) rootNode.name = pathToUtf8(root.root_name()) + pathToUtf8(root.root_directory());
    rootNode.type = EntryType::Directory;
    // "<root>/." forces lstat() to resolve a root that is itself a symlink.
    const detail::EntryStat rootStat = detail::probeEntry(std::filesystem::directory_entry(root / "."));
    rootNode.modified = rootStat.modified;
    rootNode.accessible = rootStat.ok;
    ctx.useDevice = options.stayOnFileSystem && rootStat.ok && rootStat.device != 0;
    ctx.rootDevice = rootStat.device;
    ctx.tree.nodes.push_back(std::move(rootNode));

    std::optional<StdThreadPool> ownedPool;
    if (executor == nullptr) {
        unsigned threads = options.threads;
        if (threads == 0) threads = std::clamp(std::thread::hardware_concurrency(), 1u, kMaxAutoThreads);
        try {
            ownedPool.emplace(threads);
        } catch (const std::exception& ex) {
            return std::unexpected(ScanError{ScanError::Code::Internal,
                                             std::string("Impossibile creare i thread di scansione: ") + ex.what()});
        }
        executor = &*ownedPool;
    }
    ctx.executor = executor;

    const auto interval = std::max(options.progressInterval, std::chrono::milliseconds(1));
    spawn(ctx, root, 0, kNoTop);
    for (;;) {
        if (ctx.tasks.waitFor(interval)) break;
        if (progress) progress(snapshotProgress(ctx));
    }

    if (ctx.error) return std::unexpected(*ctx.error);
    if (ctx.aborted.load())
        return std::unexpected(ScanError{ScanError::Code::Cancelled, "Scansione annullata dall'utente."});

    if (progress) {
        auto last = snapshotProgress(ctx);
        last.currentPath.clear();
        progress(last);
    }
    ctx.tree.nodes.shrink_to_fit();
    return std::move(ctx.tree);
}

} // namespace diskutilities
