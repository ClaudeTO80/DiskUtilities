/**
 * @file ScanController.cpp
 * @brief ScanController implementation.
 * @details The worker thread scans, then indexes the tree into a Snapshot and publishes it. Status updates are
 *          made under a single mutex so that readers always see a consistent ScanStatus.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/web/ScanController.h"

#include "diskutilities/analysis/Snapshot.h"
#include "diskutilities/filesystem/PathUtils.h"

#include <system_error>
#include <utility>

namespace diskutilities::web {

const char* toString(ScanState state) noexcept {
    switch (state) {
        case ScanState::Idle: return "idle";
        case ScanState::Running: return "running";
        case ScanState::Completed: return "completed";
        case ScanState::Cancelled: return "cancelled";
        case ScanState::Failed: return "failed";
    }
    return "idle";
}

ScanController::ScanController(SnapshotBridge& bridge, ScanOptions options, Executor* executor)
    : bridge_(bridge), options_(options), executor_(executor) {}

ScanController::~ScanController() {
    std::lock_guard lifecycle(lifecycleMutex_);
    // Join explicitly: worker_ would otherwise outlive the mutex/status it uses.
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

std::expected<void, std::string> ScanController::start(const std::filesystem::path& requestedRoot) {
    std::lock_guard lifecycle(lifecycleMutex_);
    {
        std::lock_guard lock(mutex_);
        if (status_.state == ScanState::Running)
            return std::unexpected("È già in corso una scansione.");
    }

    std::error_code ec;
    auto root = std::filesystem::absolute(requestedRoot, ec);
    if (ec) return std::unexpected("Impossibile risolvere il percorso: " + ec.message());
    root = root.lexically_normal();

    // A previous worker has already published its final state; this only reaps the thread.
    if (worker_.joinable()) worker_.join();

    {
        std::lock_guard lock(mutex_);
        status_ = {};
        status_.state = ScanState::Running;
        status_.phase = "scanning";
        status_.root = pathToUtf8(root);
        started_ = std::chrono::steady_clock::now();
    }
    try {
        worker_ = std::jthread([this, root](std::stop_token token) { run(root, std::move(token)); });
    } catch (const std::system_error& ex) {
        std::lock_guard lock(mutex_);
        status_.state = ScanState::Failed;
        status_.phase.clear();
        status_.error = std::string("Impossibile avviare il thread di scansione: ") + ex.what();
        idle_.notify_all();
        return std::unexpected(status_.error);
    }
    return {};
}

void ScanController::cancel() {
    std::lock_guard lifecycle(lifecycleMutex_);
    worker_.request_stop(); // no-op when no scan is running
}

ScanStatus ScanController::status() const {
    std::lock_guard lock(mutex_);
    ScanStatus copy = status_;
    if (copy.state == ScanState::Running) {
        copy.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count());
    }
    return copy;
}

void ScanController::wait() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return status_.state != ScanState::Running; });
}

void ScanController::run(std::filesystem::path root, std::stop_token token) {
    const auto finish = [this](ScanState state, std::string error) {
        std::lock_guard lock(mutex_);
        status_.state = state;
        status_.phase.clear();
        status_.currentPath.clear();
        status_.live = {}; // partial results are replaced by the published snapshot
        status_.error = std::move(error);
        status_.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count());
        idle_.notify_all();
    };

    try {
        // The capacity is needed early so the UI can show the scanned share while the scan runs.
        VolumeInfo volume;
        {
            std::error_code ec;
            const auto space = std::filesystem::space(root, ec);
            if (!ec) volume = {space.capacity, space.free, space.available, true};
            std::lock_guard lock(mutex_);
            status_.volume = volume;
        }

        DirectoryScanner scanner;
        auto result = scanner.scan(
            root, options_,
            [this](const ScanProgress& progress) {
                std::lock_guard lock(mutex_);
                status_.entriesVisited = progress.entriesVisited;
                status_.filesFound = progress.filesFound;
                status_.directoriesFound = progress.directoriesFound;
                status_.inaccessibleFound = progress.inaccessibleFound;
                status_.bytesFound = progress.bytesFound;
                status_.currentPath = progress.currentPath;
                status_.live = progress.live;
            },
            token, executor_);
        if (!result) {
            finish(result.error().code == ScanError::Code::Cancelled ? ScanState::Cancelled : ScanState::Failed,
                   result.error().message);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            status_.phase = "indexing";
            status_.currentPath.clear();
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        bridge_.publish(Snapshot::create(std::move(*result), volume, elapsed));
        finish(ScanState::Completed, {});
    } catch (const std::exception& ex) {
        finish(ScanState::Failed, ex.what());
    } catch (...) {
        finish(ScanState::Failed, "Errore sconosciuto durante la scansione.");
    }
}

} // namespace diskutilities::web
