/**
 * @file FolderBrowser.cpp
 * @brief Implementation of the read-only folder listing.
 * @details Uses only std::filesystem directory iteration with error codes: no exception is thrown for
 *          missing, unreadable or vanishing entries. The Windows branch (drive enumeration) is compiled
 *          only there and has not been exercised by the Linux test suite.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FolderBrowser.h"

#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <utility>

namespace diskutilities {
namespace {

/** Three-way ASCII case-insensitive comparison used for the display order. */
bool lessNoCase(const std::string& a, const std::string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char x = asciiLower(a[i]);
        const char y = asciiLower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b; // total order for names that differ only by case
}

#if defined(_WIN32)
/** Existing drive roots ("C:/", "D:/", ...). */
std::vector<std::filesystem::path> driveRoots() {
    std::vector<std::filesystem::path> drives;
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        const std::filesystem::path root(std::string(1, letter) + ":\\");
        std::error_code ec;
        if (std::filesystem::exists(root, ec) && !ec) drives.push_back(root);
    }
    return drives;
}
#endif

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home) return std::filesystem::path(home);
#else
    if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home);
#endif
    return {};
}

} // namespace

FolderListing listFolders(const std::filesystem::path& requested, std::size_t limit) {
    FolderListing listing;

#if defined(_WIN32)
    if (requested.empty()) {
        listing.found = true;
        listing.canGoUp = false;
        for (const auto& drive : driveRoots()) {
            const std::string path = pathToUtf8(drive);
            listing.folders.push_back({path.substr(0, 2), path, false});
        }
        return listing;
    }
#endif

    std::error_code ec;
    auto directory = requested.empty() ? std::filesystem::path("/") : std::filesystem::absolute(requested, ec);
    if (ec) {
        listing.error = "Impossibile risolvere il percorso: " + ec.message();
        return listing;
    }
    directory = directory.lexically_normal();
    if (!directory.has_filename() && directory.has_relative_path()) directory = directory.parent_path();
    listing.path = pathToUtf8(directory);

    const auto status = std::filesystem::status(directory, ec);
    if (ec || !std::filesystem::exists(status)) {
        listing.error = ec ? "Percorso non accessibile: " + ec.message() : "Il percorso non esiste.";
        return listing;
    }
    if (!std::filesystem::is_directory(status)) {
        listing.error = "Il percorso non è una cartella.";
        return listing;
    }

    // No skip_permission_denied: an unreadable folder must be reported, not shown as empty.
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, ec);
    if (ec) {
        listing.error = "Impossibile leggere la cartella: " + ec.message();
        return listing;
    }
    listing.found = true;

    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code entryError;
        const auto& entry = *it;
        if (entry.is_directory(entryError) && !entryError) {
            if (listing.folders.size() >= limit) {
                listing.truncated = true;
                break;
            }
            FolderEntry folder;
            folder.name = toValidUtf8(pathToUtf8(entry.path().filename()));
            folder.path = pathToUtf8(entry.path());
            folder.hidden = !folder.name.empty() && folder.name.front() == '.';
            listing.folders.push_back(std::move(folder));
        }
        it.increment(ec);
        if (ec) break; // a vanishing or unreadable entry ends the listing; what we have is still valid
    }
    std::sort(listing.folders.begin(), listing.folders.end(),
              [](const FolderEntry& a, const FolderEntry& b) { return lessNoCase(a.name, b.name); });

    const auto parent = directory.parent_path();
    if (!parent.empty() && parent != directory) {
        listing.parent = pathToUtf8(parent);
        listing.canGoUp = true;
    } else {
#if defined(_WIN32)
        listing.canGoUp = true; // from a drive root go to the list of drives (empty path)
#endif
    }
    return listing;
}

std::vector<FolderShortcut> folderShortcuts() {
    std::vector<FolderShortcut> shortcuts;
    std::error_code ec;
    const auto addIfDirectory = [&](std::string name, const std::filesystem::path& path) {
        if (path.empty() || !std::filesystem::is_directory(path, ec)) return;
        shortcuts.push_back({std::move(name), pathToUtf8(path)});
    };
    addIfDirectory("Cartella utente", homeDirectory());
#if defined(_WIN32)
    for (const auto& drive : driveRoots()) shortcuts.push_back({pathToUtf8(drive).substr(0, 2), pathToUtf8(drive)});
#else
    addIfDirectory("Radice (/)", "/");
    for (const char* mount : {"/mnt", "/media", "/Volumes"}) addIfDirectory(mount, mount);
#endif
    return shortcuts;
}

} // namespace diskutilities
