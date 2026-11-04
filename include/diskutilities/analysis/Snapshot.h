#pragma once

/**
 * @file Snapshot.h
 * @brief Immutable, indexed view of a completed scan.
 * @details A Snapshot is built once from a FileTree and never modified afterwards, so it can be shared between
 *          threads through std::shared_ptr<const Snapshot> without any locking. All aggregates (directory
 *          sizes, extension statistics, ordering) are precomputed in O(n log n); every query afterwards costs
 *          only what it returns.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FileTree.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace diskutilities {

/** @brief Index of a node inside Snapshot::node(). */
using NodeId = std::uint32_t;

/** @brief Capacity figures of the volume that contains the scanned root. */
struct VolumeInfo {
    std::uint64_t capacity = 0;   ///< Total size of the volume in bytes.
    std::uint64_t free = 0;       ///< Free bytes, including space reserved for privileged users.
    std::uint64_t available = 0;  ///< Free space usable by an unprivileged user.
    bool valid = false;           ///< False when the capacity could not be determined.
};

/** @brief Aggregate for one (lower-cased) file extension. */
struct ExtensionStat {
    std::string extension; ///< ".txt", or Snapshot::kNoExtension.
    std::uint64_t count = 0; ///< Number of regular files with this extension.
    std::uint64_t bytes = 0; ///< Sum of their sizes in bytes.
};

/** @brief Whole-tree counters. */
struct SnapshotTotals {
    std::uint64_t bytes = 0; ///< Sum of regular file sizes.
    std::uint64_t files = 0;       ///< Number of regular files.
    std::uint64_t directories = 0; ///< Includes the root.
    std::uint64_t symlinks = 0;    ///< Number of symbolic links (never followed).
    std::uint64_t others = 0;      ///< Sockets, devices, FIFOs and other special entries.
    std::uint64_t inaccessible = 0; ///< Entries whose metadata or listing could not be read.
};

/** @brief Sort criteria of Snapshot::list(). Directories always come first. */
enum class SortKey { Name, Size, Modified, Type };

/** @brief Parameters of Snapshot::list(). */
struct ListQuery {
    SortKey sort = SortKey::Name;  ///< Sort criterion applied inside the directory and file groups.
    bool descending = false;       ///< Reverse the sort order.
    std::string filter;            ///< Case-insensitive (ASCII) substring of the name; empty = all.
    bool directoriesOnly = false;  ///< Hide files, symlinks and special entries.
    std::size_t offset = 0;        ///< Index of the first returned entry.
    std::size_t limit = 2000;      ///< Maximum number of returned entries.
};

/** @brief One page of a directory listing. */
struct Listing {
    std::vector<NodeId> entries;        ///< The requested page.
    std::size_t total = 0;              ///< Matching entries before paging.
    std::uint64_t totalBytes = 0;       ///< Sum of the aggregate sizes of the matching entries.
};

/**
 * @brief Immutable, indexed result of a scan.
 * @details Create it with Snapshot::create() and share it as `std::shared_ptr<const Snapshot>`. All
 * accessors are const and thread-safe. Directory sizes are aggregated once at creation, children are
 * stored in CSR form (one offset array plus one id array) and every node carries a rank in a path-ordered
 * traversal so that ties between equal sizes are broken deterministically.
 */
class Snapshot final {
public:
    /** Extension label used for files without a usable extension. */
    static constexpr std::string_view kNoExtension = "senza estensione";

    /** Hard cap on the number of precomputed "largest files". */
    static constexpr std::size_t kMaxLargestFiles = 10000;

    /**
     * @brief Index a finished scan.
     * @throws std::invalid_argument if the tree violates the FileTree invariants.
     */
    [[nodiscard]] static std::shared_ptr<const Snapshot> create(
        FileTree tree, VolumeInfo volume = {},
        std::chrono::milliseconds scanDuration = std::chrono::milliseconds{0});

    /** @brief Absolute, lexically normal root path of the scan. */
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    /** @brief The root path as UTF-8 with '/' separators. */
    [[nodiscard]] const std::string& rootUtf8() const noexcept { return rootUtf8_; }
    /** @brief Whole-tree counters. */
    [[nodiscard]] const SnapshotTotals& totals() const noexcept { return totals_; }
    /** @brief Capacity of the volume that contains the root. */
    [[nodiscard]] const VolumeInfo& volume() const noexcept { return volume_; }
    /** @brief Wall-clock time the scan took. */
    [[nodiscard]] std::chrono::milliseconds scanDuration() const noexcept { return scanDuration_; }
    /** @brief Number of entries in the snapshot, root included. */
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }

    /** @brief Raw node. @p id must be smaller than nodeCount(). */
    [[nodiscard]] const Node& node(NodeId id) const { return nodes_[id]; }
    /** @brief Id of the root directory (always 0). */
    [[nodiscard]] static constexpr NodeId rootId() noexcept { return 0; }

    /** @brief Total bytes below a directory (file size for files). */
    [[nodiscard]] std::uint64_t bytesOf(NodeId id) const { return aggregateBytes_[id]; }
    /** @brief Number of regular files below a directory (1 for a file). */
    [[nodiscard]] std::uint64_t filesOf(NodeId id) const { return aggregateFiles_[id]; }
    /** @brief Direct children, sorted by name (binary order). */
    [[nodiscard]] std::span<const NodeId> children(NodeId id) const;
    /** @brief True if the directory has at least one sub-directory. */
    [[nodiscard]] bool hasSubdirectories(NodeId id) const;

    /** @brief Absolute path of a node, UTF-8 with '/' separators. */
    [[nodiscard]] std::string pathOf(NodeId id) const;
    /** @brief Lower-cased extension of a regular file, or kNoExtension. Empty for other types. */
    [[nodiscard]] std::string extensionOf(NodeId id) const;

    /** @brief Resolve an absolute path to a node; nullopt if outside the root or unknown. */
    [[nodiscard]] std::optional<NodeId> find(const std::filesystem::path& absolutePath) const;

    /** @brief The @p limit biggest regular files, size descending (ties broken by path order). */
    [[nodiscard]] std::span<const NodeId> largestFiles(std::size_t limit) const;
    /** @brief Direct sub-directories of the root ordered by aggregate size, descending. */
    [[nodiscard]] std::vector<NodeId> largestTopLevelDirectories(std::size_t limit) const;
    /** @brief Extension statistics ordered by total bytes, descending. */
    [[nodiscard]] const std::vector<ExtensionStat>& extensions() const noexcept { return extensions_; }

    /** @brief Paged, sorted, filtered listing of a directory's children. */
    [[nodiscard]] Listing list(NodeId directory, const ListQuery& query) const;

private:
    Snapshot() = default;

    std::filesystem::path root_;
    std::string rootUtf8_;
    std::vector<Node> nodes_;
    std::vector<std::uint64_t> aggregateBytes_;
    std::vector<std::uint64_t> aggregateFiles_;
    std::vector<std::uint32_t> childOffsets_; ///< CSR offsets, size nodes + 1.
    std::vector<NodeId> childIds_;            ///< CSR payload, each range sorted by name.
    std::vector<std::uint32_t> order_;        ///< Rank of every node in a path-ordered traversal.
    std::vector<NodeId> largestFiles_;
    std::vector<ExtensionStat> extensions_;
    SnapshotTotals totals_;
    VolumeInfo volume_;
    std::chrono::milliseconds scanDuration_{0};
};

} // namespace diskutilities
