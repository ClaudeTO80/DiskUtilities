/**
 * @file tk4cpp_tests.cpp
 * @brief Tests for the TK4CPP adapters.
 * @details Built only when the TK4CPP package is available.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/analysis/Snapshot.h"
#include "diskutilities/filesystem/DirectoryScanner.h"
#include "diskutilities/tk4cpp/Tk4cppSupport.h"

#include <TK4CPP/Utility/FolderHandler.hpp>
#if __has_include(<TK4CPP/Utility/Glob.hpp>)
#define TK4_SEARCH_SUPPORT 1
#include <TK4CPP/Utility/Glob.hpp>
#include <TK4CPP/Utility/SizeFormat.hpp>
#include <TK4CPP/Utility/TimeParse.hpp>
#include "diskutilities/filesystem/Glob.h"
#include "FindArguments.h"
#endif

#include <atomic>
#include <latch>

using namespace diskutilities;
namespace fs = std::filesystem;

void executorRunsTasks() {
    std::atomic<int> sum{0};
    {
        tk4cpp::Tk4cppExecutor executor(3);
        CHECK_EQ(executor.concurrency(), std::size_t{3});
        for (int i = 1; i <= 2000; ++i) executor.post([&sum, i] { sum += i; });
    } // graceful shutdown drains the queue
    CHECK_EQ(sum.load(), 2000 * 2001 / 2);
}

void executorSupportsNestedPosts() {
    tk4cpp::Tk4cppExecutor executor(2);
    std::latch done(1 + 50);
    executor.post([&] {
        for (int i = 0; i < 50; ++i) executor.post([&] { done.count_down(); });
        done.count_down();
    });
    done.wait();
    CHECK(executor.stats().totalFailed == 0);
}

void executorSurvivesThrowingTasks() {
    std::atomic<int> after{0};
    {
        tk4cpp::Tk4cppExecutor executor(1);
        executor.post([] { throw std::runtime_error("boom"); });
        executor.post([&] { ++after; });
    }
    CHECK_EQ(after.load(), 1);
}

void scannerWorksOnTk4cppPool() {
    TempDir dir;
    for (int d = 0; d < 10; ++d)
        for (int f = 0; f < 20; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 10);
    tk4cpp::Tk4cppExecutor executor;
    const auto tree = DirectoryScanner{}.scan(dir.path(), {}, {}, {}, &executor);
    CHECK(tree.has_value());
    if (!tree) return;
    CHECK_EQ(tree->nodes.size(), std::size_t{1 + 10 + 200});
    CHECK(executor.stats().totalCompleted >= 11); // root + 10 directories
}

/** FolderHandler::stats() is an independent implementation: use it as an oracle (no symlinks in the tree). */
void totalsMatchFolderHandlerStats() {
    TempDir dir;
    for (int d = 0; d < 8; ++d)
        for (int e = 0; e < 4; ++e)
            for (int f = 0; f < 6; ++f)
                makeFile(dir.path() / ("d" + std::to_string(d)) / ("e" + std::to_string(e)) / ("f" + std::to_string(f)),
                         static_cast<std::uintmax_t>(d * 1000 + e * 100 + f));
    fs::create_directories(dir.path() / "empty" / "deeper");

    const auto stats = TK4CPP::Utility::FolderHandler{}.stats(dir.path());
    auto tree = DirectoryScanner{}.scan(dir.path());
    CHECK(tree.has_value());
    if (!tree) return;
    const auto snap = Snapshot::create(std::move(*tree));
    CHECK_EQ(snap->totals().bytes, static_cast<std::uint64_t>(stats.totalSize));
    CHECK_EQ(snap->totals().files, static_cast<std::uint64_t>(stats.fileCount));
    CHECK_EQ(snap->totals().directories - 1, static_cast<std::uint64_t>(stats.folderCount)); // stats() excludes the root
}

void recommendationsAreSane() {
    const unsigned threads = tk4cpp::recommendedScanThreads();
    CHECK(threads >= 1 && threads <= 8);
    const auto limit = tk4cpp::recommendedNodeLimit();
    CHECK(limit == 0 || limit > 10'000); // either "unknown" or a plausible budget
    CHECK_EQ(tk4cpp::formatBytes(0), std::string("0 B"));
    CHECK_EQ(tk4cpp::formatBytes(1536), std::string("1.50 KB"));
}

#if defined(TK4_SEARCH_SUPPORT)
/** The TK4CPP matcher and the bundled core matcher must agree on a shared corpus (the core cannot depend on TK4CPP). */
void globParityWithCore() {
    const char* patterns[] = {"*", "*.cpp", "a?c", "file[0-9].txt", "[!abc]x", "a\\*b", "caf?", "*\xC3\xA9", "a*b*c", "[a-]x", "a[", ""};
    const char* texts[]    = {"", "main.cpp", "abc", "file7.txt", "dx", "a*b", "caf\xC3\xA9", "aXXbYYc", "-x", "a[", "x", "MAIN.CPP", "a\xFF" "b"};
    int compared = 0;
    for (const char* p : patterns)
        for (const char* t : texts)
            for (bool cs : {true, false}) {
                CHECK_EQ(TK4CPP::Utility::globMatch(p, t, cs), globMatch(p, t, cs));
                ++compared;
            }
    CHECK_EQ(compared, 12 * 13 * 2);
}

void parsersParityWithBundledVectors() {
    CHECK_EQ(*cli::parseSize("1.5MB"), std::uint64_t{1'500'000});
    CHECK(!cli::parseSize("5 MB").has_value());
    CHECK_EQ(tk4cpp::formatBytes(1536), std::string("1.50 KB"));
    CHECK_EQ(TK4CPP::Utility::formatSize(1'500'000, TK4CPP::Utility::SizeUnits::Decimal), std::string("1.50 MB"));
}
#endif

int main() {
    RUN(executorRunsTasks);
    RUN(executorSupportsNestedPosts);
    RUN(executorSurvivesThrowingTasks);
    RUN(scannerWorksOnTk4cppPool);
    RUN(totalsMatchFolderHandlerStats);
    RUN(recommendationsAreSane);
#if defined(TK4_SEARCH_SUPPORT)
    RUN(globParityWithCore);
    RUN(parsersParityWithBundledVectors);
#endif
    return testReport("tk4cpp_tests");
}
