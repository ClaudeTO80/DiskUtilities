/**
 * @file Snapshot.cpp
 * @brief Snapshot construction and queries.
 * @details create() validates the tree, aggregates sizes with one reverse pass (parents precede children),
 *          builds the CSR child lists, the path-order ranks, the extension table and the largest-file index.
 *          The query methods only read these precomputed structures.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/analysis/Snapshot.h"

#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace diskutilities {
namespace {


/** Three-way ASCII case-insensitive comparison. */
int compareNoCase(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char x = asciiLower(a[i]);
        const char y = asciiLower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool containsNoCase(std::string_view haystack, std::string_view loweredNeedle) noexcept {
    if (loweredNeedle.empty()) return true;
    if (loweredNeedle.size() > haystack.size()) return false;
    const std::size_t last = haystack.size() - loweredNeedle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t k = 0;
        while (k < loweredNeedle.size() && asciiLower(haystack[i + k]) == loweredNeedle[k]) ++k;
        if (k == loweredNeedle.size()) return true;
    }
    return false;
}


int compareInt(std::uint64_t a, std::uint64_t b) noexcept { return a < b ? -1 : (a > b ? 1 : 0); }

} // namespace

std::shared_ptr<const Snapshot> Snapshot::create(FileTree tree, VolumeInfo volume,
                                                 std::chrono::milliseconds scanDuration) {
    auto& nodes = tree.nodes;
    if (nodes.empty() || nodes[0].parent != kNoParent || nodes[0].type != EntryType::Directory)
        throw std::invalid_argument("Snapshot: il nodo 0 deve essere la directory radice");
    if (nodes.size() >= kNoParent) throw std::invalid_argument("Snapshot: troppi nodi");
    for (std::size_t i = 1; i < nodes.size(); ++i)
        if (nodes[i].parent >= i)
            throw std::invalid_argument("Snapshot: ogni nodo deve seguire il proprio genitore");

    std::shared_ptr<Snapshot> snap(new Snapshot());
    const std::size_t n = nodes.size();

    snap->root_ = std::move(tree.root);
    if (!snap->root_.has_filename() && snap->root_.has_relative_path())
        snap->root_ = snap->root_.parent_path(); // drop a trailing separator
    snap->rootUtf8_ = pathToUtf8(snap->root_);
    snap->nodes_ = std::move(nodes);
    snap->volume_ = volume;
    snap->scanDuration_ = scanDuration;
    const auto& list = snap->nodes_;

    // --- Aggregates and totals --------------------------------------------------
    snap->aggregateBytes_.assign(n, 0);
    snap->aggregateFiles_.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const Node& node = list[i];
        if (!node.accessible) ++snap->totals_.inaccessible;
        switch (node.type) {
            case EntryType::RegularFile:
                snap->aggregateBytes_[i] = node.size;
                snap->aggregateFiles_[i] = 1;
                snap->totals_.bytes += node.size;
                ++snap->totals_.files;
                break;
            case EntryType::Directory: ++snap->totals_.directories; break;
            case EntryType::Symlink: ++snap->totals_.symlinks; break;
            case EntryType::Other: ++snap->totals_.others; break;
        }
    }
    // Parents precede children, so one reverse pass folds every subtree upwards.
    for (std::size_t i = n; i-- > 1;) {
        const std::uint32_t parent = list[i].parent;
        snap->aggregateBytes_[parent] += snap->aggregateBytes_[i];
        snap->aggregateFiles_[parent] += snap->aggregateFiles_[i];
    }

    // --- Children in CSR form, each range sorted by name -----------------------
    snap->childOffsets_.assign(n + 1, 0);
    for (std::size_t i = 1; i < n; ++i) ++snap->childOffsets_[list[i].parent + 1];
    for (std::size_t i = 0; i < n; ++i) snap->childOffsets_[i + 1] += snap->childOffsets_[i];
    snap->childIds_.resize(n > 0 ? n - 1 : 0);
    {
        std::vector<std::uint32_t> cursor(snap->childOffsets_.begin(), snap->childOffsets_.end() - 1);
        for (std::size_t i = 1; i < n; ++i)
            snap->childIds_[cursor[list[i].parent]++] = static_cast<NodeId>(i);
    }
    for (std::size_t i = 0; i < n; ++i) {
        auto first = snap->childIds_.begin() + snap->childOffsets_[i];
        auto last = snap->childIds_.begin() + snap->childOffsets_[i + 1];
        std::sort(first, last, [&](NodeId a, NodeId b) { return list[a].name < list[b].name; });
    }

    // --- Path-ordered rank (iterative pre-order DFS) -----------------------------
    snap->order_.assign(n, 0);
    {
        std::vector<NodeId> stack{0};
        std::uint32_t rank = 0;
        while (!stack.empty()) {
            const NodeId id = stack.back();
            stack.pop_back();
            snap->order_[id] = rank++;
            const auto kids = snap->children(id);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
        }
    }

    // --- Extension statistics -----------------------------------------------------
    {
        std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> grouped;
        std::string key;
        for (std::size_t i = 0; i < n; ++i) {
            if (list[i].type != EntryType::RegularFile) continue;
            const auto ext = rawExtension(list[i].name);
            key.assign(ext.empty() ? kNoExtension : ext);
            if (!ext.empty()) std::transform(key.begin(), key.end(), key.begin(), asciiLower);
            auto& slot = grouped[key];
            ++slot.first;
            slot.second += list[i].size;
        }
        snap->extensions_.reserve(grouped.size());
        for (auto& [extension, value] : grouped)
            snap->extensions_.push_back({extension, value.first, value.second});
        std::sort(snap->extensions_.begin(), snap->extensions_.end(), [](const auto& a, const auto& b) {
            if (a.bytes != b.bytes) return a.bytes > b.bytes;
            return a.extension < b.extension;
        });
    }

    // --- Largest files ---------------------------------------------------------------
    {
        std::vector<NodeId> files;
        files.reserve(static_cast<std::size_t>(snap->totals_.files));
        for (std::size_t i = 0; i < n; ++i)
            if (list[i].type == EntryType::RegularFile) files.push_back(static_cast<NodeId>(i));
        const std::size_t keep = std::min(files.size(), kMaxLargestFiles);
        const auto better = [&](NodeId a, NodeId b) {
            if (list[a].size != list[b].size) return list[a].size > list[b].size;
            return snap->order_[a] < snap->order_[b];
        };
        std::partial_sort(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(keep), files.end(), better);
        files.resize(keep);
        files.shrink_to_fit();
        snap->largestFiles_ = std::move(files);
    }
    return snap;
}

std::span<const NodeId> Snapshot::children(NodeId id) const {
    return {childIds_.data() + childOffsets_[id], childOffsets_[id + 1] - childOffsets_[id]};
}

bool Snapshot::hasSubdirectories(NodeId id) const {
    for (const NodeId child : children(id))
        if (nodes_[child].type == EntryType::Directory) return true;
    return false;
}

std::string Snapshot::pathOf(NodeId id) const {
    std::vector<const std::string*> parts;
    std::size_t length = rootUtf8_.size();
    for (NodeId cur = id; cur != 0; cur = nodes_[cur].parent) {
        parts.push_back(&nodes_[cur].name);
        length += nodes_[cur].name.size() + 1;
    }
    std::string result;
    result.reserve(length);
    result = rootUtf8_;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (result.empty() || result.back() != '/') result.push_back('/');
        result += **it;
    }
    return result;
}

std::string Snapshot::extensionOf(NodeId id) const {
    const Node& node = nodes_[id];
    if (node.type != EntryType::RegularFile) return {};
    const auto ext = rawExtension(node.name);
    if (ext.empty()) return std::string(kNoExtension);
    std::string result(ext);
    std::transform(result.begin(), result.end(), result.begin(), asciiLower);
    return result;
}

std::optional<NodeId> Snapshot::find(const std::filesystem::path& absolutePath) const {
    const auto relative = absolutePath.lexically_normal().lexically_relative(root_);
    if (relative.empty()) return std::nullopt;
    NodeId current = 0;
    for (const auto& part : relative) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return std::nullopt;
        const std::string name = toValidUtf8(pathToUtf8(part));
        const auto kids = children(current);
        const auto it = std::lower_bound(kids.begin(), kids.end(), name,
                                         [&](NodeId child, const std::string& value) {
                                             return nodes_[child].name < value;
                                         });
        if (it == kids.end() || nodes_[*it].name != name) return std::nullopt;
        current = *it;
    }
    return current;
}

std::span<const NodeId> Snapshot::largestFiles(std::size_t limit) const {
    return std::span<const NodeId>(largestFiles_).first(std::min(limit, largestFiles_.size()));
}

std::vector<NodeId> Snapshot::largestTopLevelDirectories(std::size_t limit) const {
    std::vector<NodeId> result;
    for (const NodeId child : children(0))
        if (nodes_[child].type == EntryType::Directory) result.push_back(child);
    std::sort(result.begin(), result.end(), [&](NodeId a, NodeId b) {
        if (aggregateBytes_[a] != aggregateBytes_[b]) return aggregateBytes_[a] > aggregateBytes_[b];
        return order_[a] < order_[b];
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

Listing Snapshot::list(NodeId directory, const ListQuery& query) const {
    Listing listing;
    std::string needle = query.filter;
    std::transform(needle.begin(), needle.end(), needle.begin(), asciiLower);

    std::vector<NodeId> items;
    const auto kids = children(directory);
    items.reserve(kids.size());
    for (const NodeId child : kids) {
        const Node& node = nodes_[child];
        if (query.directoriesOnly && node.type != EntryType::Directory) continue;
        if (!containsNoCase(node.name, needle)) continue;
        items.push_back(child);
        listing.totalBytes += aggregateBytes_[child];
    }
    listing.total = items.size();

    // Directories first; then the requested key; ties fall back to name, then path order.
    const auto keyCompare = [&](NodeId a, NodeId b) -> int {
        const Node& x = nodes_[a];
        const Node& y = nodes_[b];
        switch (query.sort) {
            case SortKey::Name: return compareNoCase(x.name, y.name);
            case SortKey::Size: return compareInt(aggregateBytes_[a], aggregateBytes_[b]);
            case SortKey::Modified:
                if (x.modified == y.modified) return 0;
                return x.modified < y.modified ? -1 : 1;
            case SortKey::Type: return compareNoCase(rawExtension(x.name), rawExtension(y.name));
        }
        return 0;
    };
    const auto less = [&](NodeId a, NodeId b) {
        const bool aDir = nodes_[a].type == EntryType::Directory;
        const bool bDir = nodes_[b].type == EntryType::Directory;
        if (aDir != bDir) return aDir;
        int c = keyCompare(a, b);
        if (query.descending) c = -c;
        if (c != 0) return c < 0;
        c = compareNoCase(nodes_[a].name, nodes_[b].name);
        if (c != 0) return c < 0;
        return order_[a] < order_[b];
    };

    const std::size_t begin = std::min(query.offset, items.size());
    const std::size_t end = std::min(items.size(), begin + std::min(query.limit, items.size()));
    if (end < items.size()) {
        std::partial_sort(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(end), items.end(), less);
    } else {
        std::sort(items.begin(), items.end(), less);
    }
    listing.entries.assign(items.begin() + static_cast<std::ptrdiff_t>(begin),
                           items.begin() + static_cast<std::ptrdiff_t>(end));
    return listing;
}

} // namespace diskutilities
