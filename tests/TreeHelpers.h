#pragma once

/**
 * @file TreeHelpers.h
 * @brief Helpers that describe a FileTree independently of the order in which a parallel scan produced it.
 * @details Used to compare trees from different scan strategies.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FileTree.h"

#include <map>
#include <sstream>
#include <string>

/** Path of node @p index relative to the root, '/'-separated ("." for the root). */
inline std::string relPath(const diskutilities::FileTree& tree, std::size_t index) {
    if (index == 0) return ".";
    std::string result;
    for (std::size_t cur = index; cur != 0; cur = tree.nodes[cur].parent)
        result = tree.nodes[cur].name + (result.empty() ? "" : "/" + result);
    return result;
}

/** Order-independent description of a tree: "relative/path" -> "type:size". */
inline std::map<std::string, std::string> describe(const diskutilities::FileTree& tree) {
    std::map<std::string, std::string> out;
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const auto& node = tree.nodes[i];
        std::ostringstream value;
        value << static_cast<int>(node.type) << ':' << node.size << ':' << (node.accessible ? 'a' : 'x');
        out[relPath(tree, i)] = value.str();
    }
    return out;
}

/** True if every node's parent index is smaller than its own. */
inline bool parentsPrecedeChildren(const diskutilities::FileTree& tree) {
    for (std::size_t i = 1; i < tree.nodes.size(); ++i)
        if (tree.nodes[i].parent >= i) return false;
    return !tree.nodes.empty() && tree.nodes[0].parent == diskutilities::kNoParent;
}
