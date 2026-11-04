/**
 * @file scanner_tests.cpp
 * @brief Tests for DirectoryScanner.
 * @details Covers accuracy, symlinks, permissions, cancellation, the node limit, external executors and very
 *          deep trees.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"
#include "TreeHelpers.h"
#include "ManualExecutor.h"
#include "diskutilities/analysis/Snapshot.h"

#include "diskutilities/filesystem/DirectoryScanner.h"
#include "diskutilities/filesystem/EntryStat.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

using namespace diskutilities;
namespace fs = std::filesystem;

/** root/a/{x.txt 100, Y.TXT 50}, root/b/c/z.bin 1000, root/empty/, root/.hidden 10, root/noext 5 */
static void buildSampleTree(const fs::path& root) {
    makeFile(root / "a" / "x.txt", 100);
    makeFile(root / "a" / "Y.TXT", 50);
    makeFile(root / "b" / "c" / "z.bin", 1000);
    fs::create_directories(root / "empty");
    makeFile(root / ".hidden", 10);
    makeFile(root / "noext", 5);
}

static std::expected<FileTree, ScanError> scanWith(const fs::path& root, unsigned threads) {
    ScanOptions options;
    options.threads = threads;
    return DirectoryScanner{}.scan(root, options);
}

void scansExpectedContent() {
    TempDir dir;
    buildSampleTree(dir.path());
    const auto result = scanWith(dir.path(), 2);
    CHECK(result.has_value());
    if (!result) return;
    CHECK(parentsPrecedeChildren(*result));
    CHECK_EQ(result->root, dir.path());
    const auto d = describe(*result);
    // type codes: 0 file, 1 directory
    CHECK_EQ(d.at("."), std::string("1:0:a"));
    CHECK_EQ(d.at("a"), std::string("1:0:a"));
    CHECK_EQ(d.at("a/x.txt"), std::string("0:100:a"));
    CHECK_EQ(d.at("a/Y.TXT"), std::string("0:50:a"));
    CHECK_EQ(d.at("b/c/z.bin"), std::string("0:1000:a"));
    CHECK_EQ(d.at("empty"), std::string("1:0:a"));
    CHECK_EQ(d.at(".hidden"), std::string("0:10:a"));
    CHECK_EQ(d.at("noext"), std::string("0:5:a"));
    CHECK_EQ(d.size(), std::size_t{10}); // root + a,b,c,empty + 5 files
}

void resultIsIndependentOfThreadCount() {
    TempDir dir;
    // A bushy tree: 60 directories, 5 files each, three levels deep.
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 5; ++j)
            for (int k = 0; k < 2; ++k)
                for (int f = 0; f < 5; ++f)
                    makeFile(dir.path() / ("d" + std::to_string(i)) / ("e" + std::to_string(j)) /
                                 ("g" + std::to_string(k)) / ("f" + std::to_string(f) + ".dat"),
                             static_cast<std::uintmax_t>(i * 100 + j * 10 + k + f));
    const auto reference = scanWith(dir.path(), 1);
    CHECK(reference.has_value());
    if (!reference) return;
    CHECK_EQ(reference->nodes.size(), std::size_t{1 + 6 + 30 + 60 + 300});
    for (const unsigned threads : {2u, 4u, 16u}) {
        const auto other = scanWith(dir.path(), threads);
        CHECK(other.has_value());
        if (!other) continue;
        CHECK(parentsPrecedeChildren(*other));
        CHECK(describe(*other) == describe(*reference));
    }
}

#if defined(__unix__) || defined(__APPLE__)
void symlinksAreRecordedButNotFollowed() {
    TempDir dir;
    buildSampleTree(dir.path());
    fs::create_directory_symlink(dir.path() / "a", dir.path() / "linkToA");
    fs::create_directory_symlink(dir.path(), dir.path() / "loop"); // would recurse forever if followed
    fs::create_symlink(dir.path() / "noext", dir.path() / "fileLink");
    fs::create_symlink(dir.path() / "does-not-exist", dir.path() / "dangling");
    const auto result = scanWith(dir.path(), 2);
    CHECK(result.has_value());
    if (!result) return;
    const auto d = describe(*result);
    CHECK_EQ(d.at("linkToA"), std::string("2:0:a")); // type 2 = symlink, size 0
    CHECK_EQ(d.at("loop"), std::string("2:0:a"));
    CHECK_EQ(d.at("fileLink"), std::string("2:0:a"));
    CHECK_EQ(d.at("dangling"), std::string("2:0:a"));
    CHECK(d.find("linkToA/x.txt") == d.end());
    CHECK(d.find("loop/a") == d.end());
    CHECK_EQ(d.size(), std::size_t{10 + 4});
}

void rootMayBeASymlinkToADirectory() {
    TempDir dir;
    buildSampleTree(dir.path() / "real");
    fs::create_directory_symlink(dir.path() / "real", dir.path() / "alias");
    const auto result = scanWith(dir.path() / "alias", 2);
    CHECK(result.has_value());
    if (result) CHECK_EQ(result->nodes.size(), std::size_t{10});
}

void unreadableDirectoryIsMarkedInaccessible() {
    if (::geteuid() == 0) { std::cerr << "    (skipped: running as root)\n"; return; }
    TempDir dir;
    buildSampleTree(dir.path());
    fs::permissions(dir.path() / "b", fs::perms::none);
    const auto result = scanWith(dir.path(), 2);
    fs::permissions(dir.path() / "b", fs::perms::owner_all);
    CHECK(result.has_value());
    if (!result) return;
    const auto d = describe(*result);
    CHECK_EQ(d.at("b"), std::string("1:0:x")); // visible but not listable
    CHECK(d.find("b/c") == d.end());
}

void invalidUtf8NamesAreRepaired() {
    TempDir dir;
    makeFile(dir.path() / "bad\xFF.txt", 7);
    const auto result = scanWith(dir.path(), 1);
    CHECK(result.has_value());
    if (!result) return;
    bool found = false;
    for (const auto& node : result->nodes) found = found || node.name == "bad\xEF\xBF\xBD.txt";
    CHECK(found);
}

void stayOnFileSystemKeepsSingleDeviceTrees() {
    TempDir dir;
    buildSampleTree(dir.path());
    ScanOptions options;
    options.stayOnFileSystem = true;
    options.threads = 2;
    const auto restricted = DirectoryScanner{}.scan(dir.path(), options);
    const auto full = scanWith(dir.path(), 2);
    CHECK(restricted.has_value() && full.has_value());
    if (restricted && full) CHECK(describe(*restricted) == describe(*full));
}
#endif

void metadataIsCorrect() {
    TempDir dir;
    makeFile(dir.path() / "f.bin", 1234);
    const auto stamp = std::chrono::sys_seconds{std::chrono::seconds{1'600'000'000}}; // 2020-09-13
    fs::last_write_time(dir.path() / "f.bin", std::chrono::clock_cast<fs::file_time_type::clock>(stamp));
    const auto result = scanWith(dir.path(), 1);
    CHECK(result.has_value());
    if (!result) return;
    bool seen = false;
    for (const auto& node : result->nodes) {
        if (node.name != "f.bin") continue;
        seen = true;
        CHECK_EQ(node.size, std::uint64_t{1234});
        CHECK_EQ(node.modified, std::int64_t{1'600'000'000});
        CHECK(node.type == EntryType::RegularFile);
    }
    CHECK(seen);
}

void probeVariantsAgree() {
    TempDir dir;
    makeFile(dir.path() / "f.bin", 77);
    fs::create_directory(dir.path() / "d");
    for (const char* name : {"f.bin", "d"}) {
        const fs::directory_entry entry(dir.path() / name);
        const auto a = detail::probeEntry(entry);
        const auto b = detail::probeEntryPortable(entry);
        CHECK(a.ok && b.ok);
        CHECK(a.type == b.type);
        CHECK_EQ(a.size, b.size);
        CHECK_EQ(a.modified, b.modified);
    }
    CHECK(!detail::probeEntry(fs::directory_entry(dir.path() / "missing")).ok);
    CHECK(!detail::probeEntryPortable(fs::directory_entry(dir.path() / "missing")).ok);
}

void invalidRootsAreReported() {
    TempDir dir;
    const auto missing = scanWith(dir.path() / "nope", 1);
    CHECK(!missing.has_value());
    if (!missing) CHECK(missing.error().code == ScanError::Code::InvalidPath);
    makeFile(dir.path() / "file.txt", 1);
    const auto notDir = scanWith(dir.path() / "file.txt", 1);
    CHECK(!notDir.has_value());
    if (!notDir) CHECK(notDir.error().code == ScanError::Code::NotADirectory);
}

void relativeRootsAreMadeAbsolute() {
    TempDir dir;
    buildSampleTree(dir.path());
    const auto saved = fs::current_path();
    fs::current_path(dir.path());
    const auto result = scanWith("a/../b", 1);
    fs::current_path(saved);
    CHECK(result.has_value());
    if (result) {
        CHECK(result->root.is_absolute());
        CHECK_EQ(result->root, dir.path() / "b");
        CHECK_EQ(result->nodes.size(), std::size_t{3}); // b, c, z.bin
    }
}

void cancelledBeforeStart() {
    TempDir dir;
    buildSampleTree(dir.path());
    std::stop_source source;
    source.request_stop();
    const auto result = DirectoryScanner{}.scan(dir.path(), {}, {}, source.get_token());
    CHECK(!result.has_value());
    if (!result) CHECK(result.error().code == ScanError::Code::Cancelled);
}

void cancelDuringScanIsPrompt() {
    TempDir dir;
    for (int d = 0; d < 40; ++d)
        for (int f = 0; f < 100; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    std::stop_source source;
    ScanOptions options;
    options.threads = 2;
    options.progressInterval = std::chrono::milliseconds(1);
    std::atomic<int> callbacks{0};
    const auto start = std::chrono::steady_clock::now();
    const auto result = DirectoryScanner{}.scan(
        dir.path(), options, [&](const ScanProgress&) { if (++callbacks >= 1) source.request_stop(); },
        source.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // Either the cancel won the race (expected) or the scan was so quick it had already finished.
    CHECK(!result.has_value() || result->nodes.size() == 1 + 40 + 4000);
    if (!result) CHECK(result.error().code == ScanError::Code::Cancelled);
    CHECK(elapsed < std::chrono::seconds(10));
}

void progressRunsOnCallerThreadAndFinalIsExact() {
    TempDir dir;
    buildSampleTree(dir.path());
    const auto caller = std::this_thread::get_id();
    bool alwaysCaller = true;
    ScanProgress last;
    int calls = 0;
    const auto result = DirectoryScanner{}.scan(
        dir.path(), {}, [&](const ScanProgress& p) {
            alwaysCaller = alwaysCaller && std::this_thread::get_id() == caller;
            last = p;
            ++calls;
        });
    CHECK(result.has_value());
    CHECK(alwaysCaller);
    CHECK(calls >= 1);
    CHECK(last.currentPath.empty());       // the final report carries no "current" path
    CHECK_EQ(last.entriesVisited, std::uint64_t{9}); // everything but the root
    CHECK_EQ(last.filesFound, std::uint64_t{5});
    CHECK_EQ(last.directoriesFound, std::uint64_t{5}); // root, a, b, c, empty
    CHECK_EQ(last.inaccessibleFound, std::uint64_t{0});
}

/** @brief Executor that counts the posted tasks and forwards them to a StdThreadPool. */
struct CountingExecutor final : Executor {
    StdThreadPool pool{3};        ///< Pool that really runs the tasks.
    std::atomic<int> posts{0};    ///< Number of post() calls.
    void post(Task task) override { ++posts; pool.post(std::move(task)); }
    std::size_t concurrency() const noexcept override { return 3; }
};

void usesExternalExecutor() {
    TempDir dir;
    buildSampleTree(dir.path());
    CountingExecutor executor;
    const auto result = DirectoryScanner{}.scan(dir.path(), {}, {}, {}, &executor);
    CHECK(result.has_value());
    CHECK_EQ(executor.posts.load(), 5); // one task per directory: root, a, b, c, empty
}

/** @brief Executor whose post() always throws. */
struct FailingExecutor final : Executor {
    void post(Task) override { throw std::runtime_error("pool is gone"); }
    std::size_t concurrency() const noexcept override { return 1; }
};

void executorFailureBecomesAnError() {
    TempDir dir;
    buildSampleTree(dir.path());
    FailingExecutor executor;
    const auto result = DirectoryScanner{}.scan(dir.path(), {}, {}, {}, &executor);
    CHECK(!result.has_value());
    if (!result) CHECK(result.error().code == ScanError::Code::Internal);
}

/** Tree with distinct sizes, several extensions and nested directories (all ranks are unambiguous). */
static void buildLiveTree(const fs::path& root) {
    makeFile(root / "photos" / "a.JPG", 4000);
    makeFile(root / "photos" / "b.jpg", 3000);
    makeFile(root / "photos" / "raw" / "c.dng", 9000);
    makeFile(root / "docs" / "d.pdf", 2000);
    makeFile(root / "docs" / "e.txt", 700);
    makeFile(root / "docs" / "noext", 60);
    makeFile(root / "tiny" / "f.txt", 1);
    makeFile(root / "top.log", 500);
    fs::create_directories(root / "empty");
}

/** Run a scan to completion and return the last progress report (the exact final one). */
static ScanProgress finalProgress(const fs::path& root, const ScanOptions& options, FileTree* treeOut = nullptr) {
    ScanProgress last;
    auto result = DirectoryScanner{}.scan(root, options, [&](const ScanProgress& p) { last = p; });
    CHECK(result.has_value());
    if (result && treeOut) *treeOut = std::move(*result);
    return last;
}

void liveStatisticsConvergeToTheSnapshot() {
    TempDir dir;
    buildLiveTree(dir.path());
    FileTree tree;
    ScanOptions options;
    options.threads = 3;
    const ScanProgress last = finalProgress(dir.path(), options, &tree);
    const auto snapshot = Snapshot::create(std::move(tree));

    CHECK_EQ(last.bytesFound, snapshot->totals().bytes);
    CHECK_EQ(last.live.extensionCount, std::uint64_t{snapshot->extensions().size()});

    // Extensions: same set, same counts and bytes. "" is the live label of "no extension".
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> live;
    for (const auto& e : last.live.extensions)
        live[e.extension.empty() ? std::string(Snapshot::kNoExtension) : e.extension] = {e.count, e.bytes};
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> final;
    for (const auto& e : snapshot->extensions()) final[e.extension] = {e.count, e.bytes};
    CHECK(live == final);
    CHECK_EQ(live.at(".jpg").first, std::uint64_t{2}); // a.JPG + b.jpg: case-insensitive
    CHECK_EQ(live.at(".jpg").second, std::uint64_t{7000});

    // Top-level directories: same order and sizes as the snapshot ranking ("empty" has no bytes but is listed).
    const auto top = snapshot->largestTopLevelDirectories(100);
    CHECK_EQ(last.live.directories.size(), top.size());
    for (std::size_t i = 0; i < top.size() && i < last.live.directories.size(); ++i) {
        CHECK_EQ(last.live.directories[i].name, snapshot->node(top[i]).name);
        CHECK_EQ(last.live.directories[i].bytes, snapshot->bytesOf(top[i]));
        CHECK_EQ(last.live.directories[i].path, snapshot->pathOf(top[i]));
    }

    // Largest files: same paths in the same order (sizes are all different).
    const auto biggest = snapshot->largestFiles(100);
    CHECK_EQ(last.live.largestFiles.size(), biggest.size());
    for (std::size_t i = 0; i < biggest.size() && i < last.live.largestFiles.size(); ++i) {
        CHECK_EQ(last.live.largestFiles[i].path, snapshot->pathOf(biggest[i]));
        CHECK_EQ(last.live.largestFiles[i].size, snapshot->node(biggest[i]).size);
        CHECK_EQ(last.live.largestFiles[i].modified, snapshot->node(biggest[i]).modified);
    }
}

void liveLargestFilesKeepOnlyTheBiggest() {
    TempDir dir;
    for (int i = 1; i <= 25; ++i) makeFile(dir.path() / "one" / ("f" + std::to_string(i)), 100u * i); // > capacity in one directory
    for (int i = 1; i <= 25; ++i) makeFile(dir.path() / "two" / ("g" + std::to_string(i)), 100u * i + 7);
    ScanOptions options;
    options.liveLargestFiles = 5;
    const ScanProgress last = finalProgress(dir.path(), options);
    CHECK_EQ(last.live.largestFiles.size(), std::size_t{5});
    std::vector<std::uint64_t> sizes;
    for (const auto& f : last.live.largestFiles) sizes.push_back(f.size);
    CHECK(sizes == (std::vector<std::uint64_t>{2507, 2500, 2407, 2400, 2307}));
}

void liveStatisticsCanBeDisabled() {
    TempDir dir;
    buildLiveTree(dir.path());
    ScanOptions options;
    options.liveStatistics = false;
    const ScanProgress last = finalProgress(dir.path(), options);
    CHECK(last.live.extensions.empty());
    CHECK(last.live.directories.empty());
    CHECK(last.live.largestFiles.empty());
    CHECK_EQ(last.bytesFound, std::uint64_t{19261}); // the cheap counter stays available
}

void partialResultsGrowWhileTheScanRuns() {
    TempDir dir;
    buildLiveTree(dir.path());
    ManualExecutor executor;
    ScanOptions options;
    options.progressInterval = std::chrono::milliseconds{1};

    std::vector<ScanProgress> reports;
    // Each progress tick releases exactly one directory task: partial states are fully deterministic.
    const auto result = DirectoryScanner{}.scan(
        dir.path(), options,
        [&](const ScanProgress& p) { reports.push_back(p); executor.runOne(); }, {}, &executor);
    CHECK(result.has_value());
    CHECK(reports.size() >= 5);

    bool sawPartial = false;
    std::uint64_t previousBytes = 0, previousFiles = 0;
    std::size_t previousExtensions = 0;
    for (const auto& report : reports) {
        CHECK(report.bytesFound >= previousBytes);
        CHECK(report.filesFound >= previousFiles);
        CHECK(report.live.extensionCount >= previousExtensions);
        previousBytes = report.bytesFound;
        previousFiles = report.filesFound;
        previousExtensions = report.live.extensionCount;
        if (report.bytesFound > 0 && report.bytesFound < 19261) sawPartial = true;
        // The live total always agrees with what the workers have really read so far.
        std::uint64_t byExtension = 0;
        for (const auto& e : report.live.extensions) byExtension += e.bytes;
        CHECK_EQ(byExtension, report.bytesFound);
    }
    CHECK(sawPartial);
    CHECK_EQ(reports.back().bytesFound, std::uint64_t{19261});
}

void deepTreeDoesNotRecurse() {
    TempDir dir;
    fs::path p = dir.path();
    std::size_t depth = 0;
    for (; depth < 150; ++depth) {
        p /= "d";
        std::error_code ec;
        fs::create_directory(p, ec);
        if (ec) break;
    }
    makeFile(p / "leaf.txt", 1);
    const auto result = scanWith(dir.path(), 4);
    CHECK(result.has_value());
    if (result) CHECK_EQ(result->nodes.size(), depth + 2);
}

void nodeLimitAbortsTheScan() {
    TempDir dir;
    buildSampleTree(dir.path()); // 10 nodes
    ScanOptions tight;
    tight.maxNodes = 5;
    const auto limited = DirectoryScanner{}.scan(dir.path(), tight);
    CHECK(!limited.has_value());
    if (!limited) CHECK(limited.error().code == ScanError::Code::TooManyEntries);
    ScanOptions roomy;
    roomy.maxNodes = 10;
    CHECK(DirectoryScanner{}.scan(dir.path(), roomy).has_value()); // exactly at the limit is fine
}

int main() {
    RUN(scansExpectedContent);
    RUN(resultIsIndependentOfThreadCount);
#if defined(__unix__) || defined(__APPLE__)
    RUN(symlinksAreRecordedButNotFollowed);
    RUN(rootMayBeASymlinkToADirectory);
    RUN(unreadableDirectoryIsMarkedInaccessible);
    RUN(invalidUtf8NamesAreRepaired);
    RUN(stayOnFileSystemKeepsSingleDeviceTrees);
#endif
    RUN(metadataIsCorrect);
    RUN(probeVariantsAgree);
    RUN(invalidRootsAreReported);
    RUN(relativeRootsAreMadeAbsolute);
    RUN(cancelledBeforeStart);
    RUN(cancelDuringScanIsPrompt);
    RUN(progressRunsOnCallerThreadAndFinalIsExact);
    RUN(usesExternalExecutor);
    RUN(executorFailureBecomesAnError);
    RUN(deepTreeDoesNotRecurse);
    RUN(liveStatisticsConvergeToTheSnapshot);
    RUN(liveLargestFilesKeepOnlyTheBiggest);
    RUN(liveStatisticsCanBeDisabled);
    RUN(partialResultsGrowWhileTheScanRuns);
    RUN(nodeLimitAbortsTheScan);
    return testReport("scanner_tests");
}
