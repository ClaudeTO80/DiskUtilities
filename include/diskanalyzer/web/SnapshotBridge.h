#pragma once

/**
 * @file SnapshotBridge.h
 * @brief BridgeContract implementation over an immutable Snapshot.
 * @details Holds the most recently published snapshot and translates Snapshot queries into the DTOs of
 *          BridgeContract.h.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskanalyzer/analysis/Snapshot.h"
#include "diskanalyzer/web/BridgeContract.h"

#include <memory>
#include <mutex>

namespace diskanalyzer::web {

/**
 * @brief Serves queries from the most recently published Snapshot.
 * @details Publishing swaps one shared_ptr under a mutex; queries copy that pointer and
 * then run without holding any lock, so a long query never blocks a new scan
 * from publishing and a publish never invalidates a running query.
 */
class SnapshotBridge final : public BridgeContract {
public:
    SnapshotBridge() = default;

    /** @brief Make @p snapshot the data source for subsequent queries. */
    void publish(std::shared_ptr<const Snapshot> snapshot);
    /** @brief Current snapshot, or null before the first completed scan. */
    [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;

    /** @copydoc BridgeContract::getDashboard */
    DashboardDto getDashboard(const std::filesystem::path& root) override;
    /** @copydoc BridgeContract::getLargestFiles */
    std::vector<FileDto> getLargestFiles(const std::filesystem::path& root, std::size_t limit) override;
    /** @copydoc BridgeContract::getExtensionStatistics */
    std::vector<ExtensionStatisticsDto> getExtensionStatistics(const std::filesystem::path& root) override;
    /** @copydoc BridgeContract::listDirectory */
    DirectoryListingDto listDirectory(const std::filesystem::path& directory, const ListOptions& options) override;
    /** @copydoc BridgeContract::getDirectoryTree */
    DirectoryTreeDto getDirectoryTree(const std::filesystem::path& root, std::size_t maxDepth) override;

    /** @brief Format Unix seconds as ISO-8601 UTC ("" for kNoTimestamp). Exposed for tests. */
    static std::string formatTimestamp(std::int64_t unixSeconds);

private:
    /** Snapshot if it was built for @p root, otherwise null. */
    std::shared_ptr<const Snapshot> snapshotFor(const std::filesystem::path& root) const;
    static std::string typeName(EntryType type);

    mutable std::mutex mutex_;
    std::shared_ptr<const Snapshot> snapshot_;
};

} // namespace diskanalyzer::web
