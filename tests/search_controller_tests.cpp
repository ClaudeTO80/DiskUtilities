/**
 * @file search_controller_tests.cpp
 * @brief Tests for SearchController.
 * @details Covers the lifecycle (idle, running, completed, cancelled, failed), live matches while running,
 *          sorting and paging, restart discarding old results and destruction while a search is running.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/web/SearchController.h"

using namespace diskutilities;
using namespace diskutilities::web;
namespace fs = std::filesystem;

static SearchCriteria nameCriteria(std::string text) {
    SearchCriteria c;
    c.nameContains = std::move(text);
    return c;
}

void completesAndPages() {
    TempDir dir;
    makeFile(dir.path() / "a" / "one.log", 30);
    makeFile(dir.path() / "b" / "two.log", 10);
    makeFile(dir.path() / "b" / "three.log", 20);
    makeFile(dir.path() / "skip.txt", 99);
    SearchController controller;
    CHECK(controller.status().state == ScanState::Idle);
    CHECK(controller.start(dir.path(), nameCriteria(".log")).has_value());
    controller.wait();
    const auto st = controller.status();
    CHECK(st.state == ScanState::Completed);
    CHECK_EQ(st.matches, std::uint64_t{3});
    CHECK(!st.truncated);

    auto page = controller.results(0, 10, SearchSort::Size, false);
    CHECK_EQ(page.total, std::uint64_t{3});
    CHECK_EQ(page.hits.size(), std::size_t{3});
    CHECK_EQ(page.hits[0].size, std::uint64_t{10});
    CHECK_EQ(page.hits[2].size, std::uint64_t{30});
    page = controller.results(0, 10, SearchSort::Size, true);
    CHECK_EQ(page.hits[0].size, std::uint64_t{30});
    page = controller.results(1, 1, SearchSort::Size, true); // offset + limit slice
    CHECK_EQ(page.hits.size(), std::size_t{1});
    CHECK_EQ(page.hits[0].size, std::uint64_t{20});
    page = controller.results(0, 10, SearchSort::Name, false);
    CHECK_EQ(page.hits[0].name, std::string("one.log"));
    CHECK_EQ(page.hits[1].name, std::string("three.log"));
    page = controller.results(0, 10, SearchSort::Path, false);
    CHECK(page.hits[0].path < page.hits[1].path && page.hits[1].path < page.hits[2].path);
    CHECK(controller.results(5, 10).hits.empty());
    CHECK(controller.results(0, 0).hits.empty());
    CHECK_EQ(controller.results(5, 10).total, std::uint64_t{3});
}

void restartDiscardsPreviousResults() {
    TempDir dir;
    makeFile(dir.path() / "x.dat", 1);
    makeFile(dir.path() / "y.bin", 1);
    SearchController controller;
    CHECK(controller.start(dir.path(), nameCriteria(".dat")).has_value());
    controller.wait();
    CHECK_EQ(controller.results(0, 10).total, std::uint64_t{1});
    CHECK(controller.start(dir.path(), nameCriteria(".bin")).has_value());
    controller.wait();
    const auto page = controller.results(0, 10);
    CHECK_EQ(page.total, std::uint64_t{1});
    CHECK_EQ(page.hits.at(0).name, std::string("y.bin"));
}

void failureAndInvalidCriteria() {
    TempDir dir;
    SearchController controller;
    CHECK(controller.start(dir.path() / "missing", {}).has_value()); // failure is asynchronous
    controller.wait();
    CHECK(controller.status().state == ScanState::Failed);
    CHECK(!controller.status().error.empty());

    SearchCriteria bad;
    bad.minSize = 9;
    bad.maxSize = 1;
    const auto r = controller.start(dir.path(), bad);
    CHECK(!r.has_value()); // rejected synchronously, nothing started
    CHECK(controller.status().state == ScanState::Failed); // previous status untouched
}

void liveMatchesAreVisibleWhileRunningAndCancelKeepsThem() {
    TempDir dir;
    for (int d = 0; d < 40; ++d)
        for (int f = 0; f < 40; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    SearchController controller;
    SearchOptions options;
    options.threads = 1; // slow enough to observe a partial state
    CHECK(controller.start(dir.path(), {}, options).has_value());
    CHECK(!controller.start(dir.path(), {}).has_value()); // already running
    bool sawPartial = false;
    for (int i = 0; i < 2000 && controller.status().state == ScanState::Running; ++i) {
        const auto page = controller.results(0, 5);
        if (page.total > 0 && page.total < 1600) { sawPartial = true; controller.cancel(); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    controller.wait();
    const auto st = controller.status();
    if (sawPartial) {
        CHECK(st.state == ScanState::Cancelled || st.state == ScanState::Completed);
        CHECK(controller.results(0, 5).total > 0); // found-so-far matches survive a cancel
    } else {
        std::cerr << "  note: search finished before a partial state could be observed\n";
    }
}

void destructionWhileRunningIsClean() {
    TempDir dir;
    for (int d = 0; d < 40; ++d)
        for (int f = 0; f < 40; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    {
        SearchController controller;
        CHECK(controller.start(dir.path(), {}).has_value());
    } // destructor cancels and joins
    CHECK(true);
}

int main() {
    RUN(completesAndPages);
    RUN(restartDiscardsPreviousResults);
    RUN(failureAndInvalidCriteria);
    RUN(liveMatchesAreVisibleWhileRunningAndCancelKeepsThem);
    RUN(destructionWhileRunningIsClean);
    return testReport("search_controller_tests");
}
