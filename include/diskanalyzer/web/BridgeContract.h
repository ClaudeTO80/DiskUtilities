#pragma once

/**
 * @file BridgeContract.h
 * @brief Transport-neutral DTOs and the query contract expected by web/app.js.
 * @details The contract is deliberately read-only: it has no delete/move/rename/write operation. Scans are
 *          started through ScanController, not through this interface, so queries never block the UI thread.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskanalyzer/analysis/Snapshot.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace diskanalyzer::web {

/**
 * @brief One row of a directory listing.
 * @details Field names are the JSON keys read by web/app.js.
 */
struct DirectoryEntryDto {
    std::string name;       ///< File or directory name (UTF-8, no path).
    std::string path;       ///< Absolute path (UTF-8, '/' separators).
    std::string type;       ///< "directory", "file", "symlink" or "other".
    std::string extension;  ///< Empty for anything but regular files.
    std::uint64_t size = 0; ///< Aggregate size for directories.
    std::uint64_t fileCount = 0; ///< Files below a directory (0 for other types).
    std::string modified;   ///< ISO-8601 UTC; empty if unavailable.
    bool accessible = true; ///< False if metadata or listing could not be read.
    bool hasSubdirectories = false; ///< Directories only; lets the tree show an expander.
};

/** @brief A directory together with its aggregate size (dashboard bars). */
struct DirectorySizeDto {
    std::string name;       ///< Directory name.
    std::string path;       ///< Absolute path (UTF-8).
    std::uint64_t size = 0; ///< Aggregate size in bytes.
};

/** @brief Capacity of the volume that contains the scanned root. */
struct VolumeDto {
    bool valid = false;             ///< False when the capacity could not be determined.
    std::uint64_t capacity = 0;     ///< Total bytes.
    std::uint64_t free = 0;         ///< Free bytes.
    std::uint64_t available = 0;    ///< Bytes available to an unprivileged user.
};

/**
 * @brief Everything the dashboard page needs in one round trip.
 * @details When no snapshot exists for the requested root only DashboardDto::scanned (false) and
 * DashboardDto::root are meaningful.
 */
struct DashboardDto {
    bool scanned = false;        ///< False when no snapshot exists for the requested root.
    std::string root;            ///< Root of the snapshot actually served (UTF-8).
    std::uint64_t totalBytes = 0;        ///< Sum of the regular file sizes.
    std::uint64_t fileCount = 0;         ///< Number of regular files.
    std::uint64_t directoryCount = 0;    ///< Number of directories, root included.
    std::uint64_t symlinkCount = 0;      ///< Number of symbolic links.
    std::uint64_t inaccessibleCount = 0; ///< Entries that could not be read.
    std::uint64_t scanDurationMs = 0;    ///< Scan duration in milliseconds.
    VolumeDto volume;                    ///< Volume capacity.
    std::vector<DirectorySizeDto> largestDirectories; ///< Direct children of the root, biggest first.
};

/** @brief Aggregate for one file extension. */
struct ExtensionStatisticsDto {
    std::string extension; ///< "senza estensione" for extensionless files.
    std::uint64_t count = 0;      ///< Number of files.
    std::uint64_t totalBytes = 0; ///< Sum of their sizes.
};

/** @brief Node of the (depth-limited) directory tree shown in the sidebar. */
struct DirectoryTreeDto {
    std::string name;       ///< Directory name.
    std::string path;       ///< Absolute path (UTF-8).
    std::uint64_t size = 0; ///< Aggregate size in bytes.
    std::vector<DirectoryTreeDto> children; ///< Sub-directories down to the requested depth.
};

/** @brief One entry of the "largest files" ranking. */
struct FileDto {
    std::string name;       ///< File name.
    std::string path;       ///< Absolute path (UTF-8).
    std::string extension;  ///< Lower-cased extension or the "no extension" label.
    std::uint64_t size = 0; ///< Size in bytes.
    std::string modified;   ///< ISO-8601 UTC; empty if unavailable.
};

/** @brief Paging, sorting and filtering options for listDirectory(). */
struct ListOptions {
    SortKey sort = SortKey::Name; ///< Sort criterion.
    bool descending = false;      ///< Reverse the order.
    std::string filter;           ///< Case-insensitive substring of the name; empty = all.
    bool directoriesOnly = false; ///< Hide everything that is not a directory.
    std::size_t offset = 0;       ///< Index of the first returned entry.
    std::size_t limit = 2000;     ///< Page size.
};

/**
 * @brief One page of a directory listing.
 * @details Use DirectoryListingDto::truncated and DirectoryListingDto::offset to request the next page.
 */
struct DirectoryListingDto {
    bool found = false;           ///< False if the directory is not part of the snapshot.
    std::string path;             ///< Absolute path of the listed directory (UTF-8).
    std::size_t total = 0;        ///< Entries matching the filter, before paging.
    std::uint64_t totalBytes = 0; ///< Aggregate size of the matching entries.
    std::size_t offset = 0;       ///< Offset of the first entry of this page.
    bool truncated = false;       ///< True if more entries exist beyond this page.
    std::vector<DirectoryEntryDto> entries; ///< The requested page.
};

/**
 * @brief Queries available to the frontend.
 * @details Implementations must be thread-safe: the WebUI dispatches each event on its
 * own thread. Every query that takes a root returns an empty/"not scanned"
 * result when the requested root differs from the one currently scanned.
 */
class BridgeContract {
public:
    virtual ~BridgeContract() = default;

    /**
     * @brief Totals, volume capacity and biggest top-level directories.
     * @param root Scan root the caller believes is current.
     * @return The dashboard, or a DashboardDto with `scanned == false`.
     */
    virtual DashboardDto getDashboard(const std::filesystem::path& root) = 0;
    /**
     * @brief The biggest regular files, size descending.
     * @param root  Scan root the caller believes is current.
     * @param limit Maximum number of files returned.
     */
    virtual std::vector<FileDto> getLargestFiles(const std::filesystem::path& root, std::size_t limit) = 0;
    /**
     * @brief Per-extension statistics, total bytes descending.
     * @param root Scan root the caller believes is current.
     */
    virtual std::vector<ExtensionStatisticsDto> getExtensionStatistics(const std::filesystem::path& root) = 0;
    /**
     * @brief One page of a directory's children.
     * @param directory Directory to list (must be inside the snapshot).
     * @param options   Sorting, filtering and paging.
     */
    virtual DirectoryListingDto listDirectory(const std::filesystem::path& directory,
                                              const ListOptions& options) = 0;
    /**
     * @brief Depth-limited tree of sub-directories.
     * @param root     Scan root the caller believes is current.
     * @param maxDepth Levels of sub-directories to include below the root (0 = root only).
     */
    virtual DirectoryTreeDto getDirectoryTree(const std::filesystem::path& root, std::size_t maxDepth) = 0;
};

} // namespace diskanalyzer::web
