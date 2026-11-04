/**
 * @file SnapshotBridge.cpp
 * @brief SnapshotBridge: translation of Snapshot queries into DTOs.
 * @details Also contains the allocation-free civil-calendar conversion used to format modification times.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/web/SnapshotBridge.h"

#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace diskutilities::web {
namespace {

/** Normalise like Snapshot does: absolute, lexically normal, no trailing separator. */
std::filesystem::path normalizeRoot(const std::filesystem::path& path) {
    auto result = path.lexically_normal();
    if (!result.has_filename() && result.has_relative_path()) result = result.parent_path();
    return result;
}

std::string joinUtf8(const std::string& base, const std::string& name) {
    std::string result = base;
    if (!result.empty() && result.back() != '/') result.push_back('/');
    result += name;
    return result;
}

DirectoryEntryDto makeEntry(const Snapshot& snapshot, NodeId id, const std::string& parentPath) {
    const Node& node = snapshot.node(id);
    DirectoryEntryDto dto;
    dto.name = node.name;
    dto.path = joinUtf8(parentPath, node.name);
    switch (node.type) {
        case EntryType::RegularFile: dto.type = "file"; break;
        case EntryType::Directory: dto.type = "directory"; break;
        case EntryType::Symlink: dto.type = "symlink"; break;
        case EntryType::Other: dto.type = "other"; break;
    }
    dto.extension = snapshot.extensionOf(id);
    dto.size = snapshot.bytesOf(id);
    if (node.type == EntryType::Directory) {
        dto.fileCount = snapshot.filesOf(id);
        dto.hasSubdirectories = snapshot.hasSubdirectories(id);
    }
    dto.modified = SnapshotBridge::formatTimestamp(node.modified);
    dto.accessible = node.accessible;
    return dto;
}

FileDto makeFile(const Snapshot& snapshot, NodeId id) {
    const Node& node = snapshot.node(id);
    return {node.name, snapshot.pathOf(id), snapshot.extensionOf(id), node.size,
            SnapshotBridge::formatTimestamp(node.modified)};
}

DirectoryTreeDto buildTree(const Snapshot& snapshot, NodeId id, std::size_t depthLeft) {
    DirectoryTreeDto node;
    node.name = snapshot.node(id).name;
    node.path = snapshot.pathOf(id);
    node.size = snapshot.bytesOf(id);
    if (depthLeft == 0) return node;
    for (const NodeId child : snapshot.children(id))
        if (snapshot.node(child).type == EntryType::Directory)
            node.children.push_back(buildTree(snapshot, child, depthLeft - 1));
    return node;
}

} // namespace

void SnapshotBridge::publish(std::shared_ptr<const Snapshot> snapshot) {
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(snapshot);
}

std::shared_ptr<const Snapshot> SnapshotBridge::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::shared_ptr<const Snapshot> SnapshotBridge::snapshotFor(const std::filesystem::path& root) const {
    auto current = snapshot();
    if (!current || normalizeRoot(root) != current->root()) return nullptr;
    return current;
}

std::string SnapshotBridge::formatTimestamp(std::int64_t unixSeconds) {
    if (unixSeconds == kNoTimestamp) return {};
    // Civil-from-days (H. Hinnant), valid for the whole proleptic Gregorian calendar.
    std::int64_t days = unixSeconds / 86400;
    std::int64_t secondsOfDay = unixSeconds % 86400;
    if (secondsOfDay < 0) { secondsOfDay += 86400; --days; }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t dayOfEra = days - era * 146097;
    const std::int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t mp = (5 * dayOfYear + 2) / 153;
    const std::int64_t day = dayOfYear - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(secondsOfDay / 3600), static_cast<long long>(secondsOfDay % 3600 / 60),
                  static_cast<long long>(secondsOfDay % 60));
    return buffer.data();
}

DashboardDto SnapshotBridge::getDashboard(const std::filesystem::path& root) {
    DashboardDto dto;
    const auto snap = snapshotFor(root);
    if (!snap) {
        dto.root = pathToUtf8(normalizeRoot(root));
        return dto;
    }
    const auto& totals = snap->totals();
    dto.scanned = true;
    dto.root = snap->rootUtf8();
    dto.totalBytes = totals.bytes;
    dto.fileCount = totals.files;
    dto.directoryCount = totals.directories;
    dto.symlinkCount = totals.symlinks;
    dto.inaccessibleCount = totals.inaccessible;
    dto.scanDurationMs = static_cast<std::uint64_t>(snap->scanDuration().count());
    dto.volume = {snap->volume().valid, snap->volume().capacity, snap->volume().free, snap->volume().available};
    for (const NodeId id : snap->largestTopLevelDirectories(8))
        dto.largestDirectories.push_back({snap->node(id).name, snap->pathOf(id), snap->bytesOf(id)});
    return dto;
}

std::vector<FileDto> SnapshotBridge::getLargestFiles(const std::filesystem::path& root, std::size_t limit) {
    std::vector<FileDto> result;
    const auto snap = snapshotFor(root);
    if (!snap) return result;
    const auto ids = snap->largestFiles(limit);
    result.reserve(ids.size());
    for (const NodeId id : ids) result.push_back(makeFile(*snap, id));
    return result;
}

std::vector<ExtensionStatisticsDto> SnapshotBridge::getExtensionStatistics(const std::filesystem::path& root) {
    std::vector<ExtensionStatisticsDto> result;
    const auto snap = snapshotFor(root);
    if (!snap) return result;
    result.reserve(snap->extensions().size());
    for (const auto& stat : snap->extensions()) result.push_back({stat.extension, stat.count, stat.bytes});
    return result;
}

DirectoryListingDto SnapshotBridge::listDirectory(const std::filesystem::path& directory,
                                                  const ListOptions& options) {
    DirectoryListingDto dto;
    dto.path = pathToUtf8(normalizeRoot(directory));
    dto.offset = options.offset;
    const auto snap = snapshot();
    if (!snap) return dto;
    const auto id = snap->find(directory);
    if (!id || snap->node(*id).type != EntryType::Directory) return dto;

    ListQuery query;
    query.sort = options.sort;
    query.descending = options.descending;
    query.filter = options.filter;
    query.directoriesOnly = options.directoriesOnly;
    query.offset = options.offset;
    query.limit = options.limit;
    const Listing listing = snap->list(*id, query);

    dto.found = true;
    dto.path = snap->pathOf(*id);
    dto.total = listing.total;
    dto.totalBytes = listing.totalBytes;
    dto.truncated = options.offset + listing.entries.size() < listing.total;
    dto.entries.reserve(listing.entries.size());
    for (const NodeId child : listing.entries) dto.entries.push_back(makeEntry(*snap, child, dto.path));
    return dto;
}

DirectoryTreeDto SnapshotBridge::getDirectoryTree(const std::filesystem::path& root, std::size_t maxDepth) {
    const auto snap = snapshotFor(root);
    if (!snap) {
        const auto normalized = normalizeRoot(root);
        const auto name = pathToUtf8(normalized.filename());
        return {name.empty() ? pathToUtf8(normalized) : name, pathToUtf8(normalized), 0, {}};
    }
    return buildTree(*snap, Snapshot::rootId(), maxDepth);
}

} // namespace diskutilities::web
