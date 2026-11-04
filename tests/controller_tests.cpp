/**
 * @file controller_tests.cpp
 * @brief Tests for ScanController.
 * @details Uses a gated executor to make the "scan in progress" state deterministic.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"
#include "ManualExecutor.h"

#include "diskutilities/web/ScanController.h"

#include <atomic>
#include <condition_variable>
#include <thread>

using namespace diskutilities;
using namespace diskutilities::web;
namespace fs = std::filesystem;

static void sampleTree(const fs::path& root) {
    makeFile(root / "a" / "x", 10);
    makeFile(root / "b" / "y", 20);
}

/** Executor whose tasks do not run until open() is called: makes "scan in progress" deterministic. */
struct GatedExecutor final : Executor {
    StdThreadPool pool{2};        ///< Pool that really runs the tasks.
    std::mutex m;                 ///< Guards #opened.
    std::condition_variable cv;   ///< Signalled by open().
    bool opened = false;          ///< True once tasks may run.
    void post(Task task) override {
        pool.post([this, task = std::move(task)]() mutable {
            std::unique_lock lock(m);
            cv.wait(lock, [this] { return opened; });
            lock.unlock();
            task();
        });
    }
    std::size_t concurrency() const noexcept override { return 2; }
    /** @brief Release every waiting and future task. */
    void open() { { std::lock_guard l(m); opened = true; } cv.notify_all(); }
};

void completedScanPublishesSnapshot() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    ScanController controller(bridge);
    CHECK(controller.status().state == ScanState::Idle);
    CHECK(controller.start(dir.path()).has_value());
    controller.wait();
    const auto status = controller.status();
    CHECK(status.state == ScanState::Completed);
    CHECK(status.error.empty());
    CHECK(status.phase.empty());
    CHECK_EQ(status.root, dir.path().generic_string());
    CHECK(bridge.getDashboard(dir.path()).scanned);
    CHECK_EQ(bridge.getDashboard(dir.path()).totalBytes, std::uint64_t{30});
    CHECK(bridge.getDashboard(dir.path()).volume.valid);
}

void secondStartWhileRunningIsRejected() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    GatedExecutor gate;
    ScanController controller(bridge, {}, &gate);
    CHECK(controller.start(dir.path()).has_value());
    CHECK(controller.status().state == ScanState::Running);
    const auto again = controller.start(dir.path());
    CHECK(!again.has_value());
    gate.open();
    controller.wait();
    CHECK(controller.status().state == ScanState::Completed);
    // After completion a new scan is accepted again.
    CHECK(controller.start(dir.path()).has_value());
    controller.wait();
    CHECK(controller.status().state == ScanState::Completed);
}

void cancelKeepsPreviousSnapshot() {
    TempDir first, second;
    sampleTree(first.path());
    sampleTree(second.path());
    SnapshotBridge bridge;
    {
        ScanController quick(bridge);
        CHECK(quick.start(first.path()).has_value());
        quick.wait();
    }
    CHECK(bridge.getDashboard(first.path()).scanned);

    GatedExecutor gate;
    ScanController controller(bridge, {}, &gate);
    CHECK(controller.start(second.path()).has_value());
    controller.cancel();
    gate.open();
    controller.wait();
    CHECK(controller.status().state == ScanState::Cancelled);
    CHECK(bridge.getDashboard(first.path()).scanned);   // old snapshot untouched
    CHECK(!bridge.getDashboard(second.path()).scanned); // the cancelled one never published
}

void failureIsReported() {
    TempDir dir;
    SnapshotBridge bridge;
    ScanController controller(bridge);
    CHECK(controller.start(dir.path() / "missing").has_value()); // failure is asynchronous
    controller.wait();
    const auto status = controller.status();
    CHECK(status.state == ScanState::Failed);
    CHECK(!status.error.empty());
    // The controller stays usable.
    sampleTree(dir.path());
    CHECK(controller.start(dir.path()).has_value());
    controller.wait();
    CHECK(controller.status().state == ScanState::Completed);
    CHECK(controller.status().error.empty());
}

void progressIsVisibleWhileRunning() {
    TempDir dir;
    for (int d = 0; d < 20; ++d)
        for (int f = 0; f < 50; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    SnapshotBridge bridge;
    ScanOptions options;
    options.progressInterval = std::chrono::milliseconds(1);
    ScanController controller(bridge, options);
    CHECK(controller.start(dir.path()).has_value());
    std::uint64_t maxSeen = 0;
    while (controller.status().state == ScanState::Running) {
        maxSeen = std::max(maxSeen, controller.status().entriesVisited);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    controller.wait();
    CHECK(controller.status().state == ScanState::Completed);
    CHECK(controller.status().entriesVisited >= maxSeen);
    CHECK_EQ(controller.status().entriesVisited, std::uint64_t{20 + 1000});
}

void destroyingWhileRunningDoesNotHang() {
    TempDir dir;
    for (int d = 0; d < 30; ++d)
        for (int f = 0; f < 50; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    SnapshotBridge bridge;
    {
        ScanController controller(bridge);
        CHECK(controller.start(dir.path()).has_value());
    } // destructor cancels and joins
    CHECK(true);
}

void concurrentStartsAllowExactlyOne() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    GatedExecutor gate;
    ScanController controller(bridge, {}, &gate);
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] { if (controller.start(dir.path())) ++accepted; });
    for (auto& t : threads) t.join();
    CHECK_EQ(accepted.load(), 1);
    gate.open();
    controller.wait();
}

void statusExposesPartialResultsWhileRunning() {
    TempDir dir;
    makeFile(dir.path() / "a" / "x.txt", 10);
    makeFile(dir.path() / "b" / "y.bin", 20000);
    makeFile(dir.path() / "c" / "z.bin", 300);
    SnapshotBridge bridge;
    ManualExecutor executor;
    ScanOptions options;
    options.progressInterval = std::chrono::milliseconds{1};
    ScanController controller(bridge, options, &executor);
    CHECK(controller.start(dir.path()).has_value());

    bool sawPartial = false;
    while (controller.status().state == ScanState::Running) {
        executor.runOne();
        std::this_thread::sleep_for(std::chrono::milliseconds{15}); // let the scan thread publish progress
        const auto status = controller.status();
        if (status.state == ScanState::Running && status.bytesFound > 0 && status.bytesFound < 20310 &&
            !status.live.largestFiles.empty())
            sawPartial = true;
    }
    CHECK(sawPartial);

    const auto done = controller.status();
    CHECK(done.state == ScanState::Completed);
    CHECK_EQ(done.bytesFound, std::uint64_t{20310});
    CHECK(done.live.largestFiles.empty()); // partial results are dropped once the snapshot is published
    CHECK(bridge.snapshot() != nullptr);
}

void stateNames() {
    CHECK_EQ(std::string(toString(ScanState::Idle)), std::string("idle"));
    CHECK_EQ(std::string(toString(ScanState::Running)), std::string("running"));
    CHECK_EQ(std::string(toString(ScanState::Completed)), std::string("completed"));
    CHECK_EQ(std::string(toString(ScanState::Cancelled)), std::string("cancelled"));
    CHECK_EQ(std::string(toString(ScanState::Failed)), std::string("failed"));
}

int main() {
    RUN(stateNames);
    RUN(statusExposesPartialResultsWhileRunning);
    RUN(completedScanPublishesSnapshot);
    RUN(secondStartWhileRunningIsRejected);
    RUN(cancelKeepsPreviousSnapshot);
    RUN(failureIsReported);
    RUN(progressIsVisibleWhileRunning);
    RUN(destroyingWhileRunningDoesNotHang);
    RUN(concurrentStartsAllowExactlyOne);
    return testReport("controller_tests");
}
