#pragma once

/**
 * @file FolderBrowser.h
 * @brief Read-only listing of sub-folders, used by the "choose a folder" dialog.
 * @details A browser cannot hand an absolute path to a web page, so the user interface asks the native
 *          side for the sub-folders of a directory and lets the user walk the tree. Only directories are
 *          returned, nothing is ever created, changed or deleted, and file contents are never read.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace diskanalyzer {

/** @brief Upper bound on the folders returned for one directory (the rest is reported as truncated). */
inline constexpr std::size_t kMaxFolderEntries = 5000;

/** @brief One sub-folder of a listed directory. */
struct FolderEntry {
    std::string name;         ///< Folder name (UTF-8, no path).
    std::string path;         ///< Absolute path (UTF-8, '/' separators).
    bool hidden = false;      ///< Name starts with a dot (POSIX convention).
};

/** @brief A well-known starting point: home folder, file-system root, a drive, a mount directory. */
struct FolderShortcut {
    std::string name;         ///< Label shown to the user.
    std::string path;         ///< Absolute path (UTF-8, '/' separators).
};

/** @brief Result of listFolders(). */
struct FolderListing {
    bool found = false;               ///< False if the path does not exist or is not a readable folder.
    std::string path;                 ///< Normalised path that was listed ("" for the list of drives on Windows).
    std::string parent;               ///< Path of the enclosing folder; "" when canGoUp is false or at a drive root.
    bool canGoUp = false;             ///< True if a "parent" navigation makes sense from here.
    bool truncated = false;           ///< True if more than the allowed number of folders exist.
    std::string error;                ///< Human readable (Italian) reason when found is false.
    std::vector<FolderEntry> folders; ///< Sub-folders, case-insensitive name order.
};

/**
 * @brief List the sub-folders of @p directory.
 * @details Symbolic links that point to a directory are listed (as in any file chooser) but are never
 * followed by the scanner. Names are repaired to valid UTF-8. An empty path means the top of the file
 * system: "/" on POSIX, the list of existing drives on Windows.
 * @param directory Folder to list; relative paths are made absolute.
 * @param limit     Maximum number of folders returned (the result is flagged as truncated beyond it).
 * @return The listing; FolderListing::found is false and FolderListing::error set on failure.
 */
[[nodiscard]] FolderListing listFolders(const std::filesystem::path& directory,
                                        std::size_t limit = kMaxFolderEntries);

/**
 * @brief Starting points for the folder chooser.
 * @details Home folder (HOME / USERPROFILE), the file-system root and, when present, common mount
 * directories on POSIX or every existing drive letter on Windows.
 * @return The shortcuts that exist on this machine, most useful first.
 */
[[nodiscard]] std::vector<FolderShortcut> folderShortcuts();

} // namespace diskanalyzer
