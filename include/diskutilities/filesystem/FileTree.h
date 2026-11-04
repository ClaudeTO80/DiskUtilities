#pragma once

/**
 * @file FileTree.h
 * @brief Compact in-memory representation of a scanned directory tree.
 * @details The tree is a flat vector of small nodes linked by parent indices. Absolute paths are never stored,
 *          which keeps the footprint per entry low enough to scan tens of millions of files.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace diskutilities {

/** @brief Kind of filesystem entry. Symbolic links are never followed. */
enum class EntryType : std::uint8_t { RegularFile, Directory, Symlink, Other };

/** @brief Sentinel stored in Node::modified when the timestamp is unavailable. */
inline constexpr std::int64_t kNoTimestamp = std::numeric_limits<std::int64_t>::min();

/** @brief Sentinel parent index of the root node. */
inline constexpr std::uint32_t kNoParent = std::numeric_limits<std::uint32_t>::max();

/**
 * @brief One entry of the scanned tree.
 * @details The absolute path is intentionally *not* stored: it is rebuilt from the
 * parent chain on demand, which keeps the per-entry memory footprint small.
 */
struct Node {
    std::string name;                     ///< UTF-8 file name (not a full path).
    std::uint64_t size = 0;               ///< Logical size in bytes (regular files only).
    std::int64_t modified = kNoTimestamp; ///< Seconds since the Unix epoch, or kNoTimestamp.
    std::uint32_t parent = kNoParent;     ///< Index of the parent directory in FileTree::nodes.
    EntryType type = EntryType::Other;    ///< Kind of entry (symbolic links are not followed).
    bool accessible = true;               ///< False if metadata or directory listing failed.
};

/**
 * @brief Result of a scan.
 * @details Invariants: nodes[0] is the root directory, and every node's parent index is
 * strictly smaller than the node's own index (parents always precede their
 * children). Order among siblings is unspecified.
 */
struct FileTree {
    std::filesystem::path root; ///< Absolute, lexically normal root path.
    std::vector<Node> nodes;    ///< Flat node array; see the invariants above.
};

} // namespace diskutilities
