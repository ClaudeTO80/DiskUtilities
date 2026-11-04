/**
 * @file folder_tests.cpp
 * @brief Tests for the read-only folder browser.
 * @details Covers ordering, hidden folders, files being excluded, symlinks, missing and non-directory paths,
 *          the entry limit, parent navigation, invalid UTF-8 names and unreadable directories.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/filesystem/FolderBrowser.h"
#include "diskutilities/filesystem/PathUtils.h"

#include <unistd.h>

using namespace diskutilities;
namespace fs = std::filesystem;

static std::vector<std::string> names(const FolderListing& listing) {
    std::vector<std::string> result;
    for (const auto& f : listing.folders) result.push_back(f.name);
    return result;
}

void listsOnlyFoldersSortedWithoutCase() {
    TempDir dir;
    fs::create_directories(dir.path() / "beta");
    fs::create_directories(dir.path() / "Alpha");
    fs::create_directories(dir.path() / "alpha2");
    fs::create_directories(dir.path() / ".hidden");
    makeFile(dir.path() / "file.txt", 5);
    const auto listing = listFolders(dir.path());
    CHECK(listing.found);
    CHECK_EQ(listing.path, pathToUtf8(dir.path()));
    CHECK(names(listing) == (std::vector<std::string>{".hidden", "Alpha", "alpha2", "beta"}));
    CHECK(listing.folders[0].hidden);
    CHECK(!listing.folders[1].hidden);
    CHECK_EQ(listing.folders[1].path, pathToUtf8(dir.path() / "Alpha"));
    CHECK(!listing.truncated);
}

void parentNavigation() {
    TempDir dir;
    fs::create_directories(dir.path() / "a" / "b");
    const auto listing = listFolders(dir.path() / "a" / "b");
    CHECK(listing.found);
    CHECK(listing.canGoUp);
    CHECK_EQ(listing.parent, pathToUtf8(dir.path() / "a"));
    const auto top = listFolders("/");
    CHECK(top.found);
    CHECK(!top.canGoUp);
    CHECK(top.parent.empty());
    CHECK(!top.folders.empty());
}

void trailingSeparatorsAndDotsAreNormalised() {
    TempDir dir;
    fs::create_directories(dir.path() / "a");
    const auto listing = listFolders(dir.path().string() + "/a/../");
    CHECK(listing.found);
    CHECK_EQ(listing.path, pathToUtf8(dir.path()));
    CHECK(names(listing) == (std::vector<std::string>{"a"}));
}

void emptyPathMeansTheTopOfTheFileSystem() {
    const auto listing = listFolders({});
    CHECK(listing.found);
    CHECK_EQ(listing.path, std::string("/"));
}

void missingAndNonDirectoryPathsAreReported() {
    TempDir dir;
    makeFile(dir.path() / "f.txt", 1);
    const auto missing = listFolders(dir.path() / "nope");
    CHECK(!missing.found);
    CHECK(!missing.error.empty());
    CHECK(missing.folders.empty());
    const auto file = listFolders(dir.path() / "f.txt");
    CHECK(!file.found);
    CHECK_EQ(file.error, std::string("Il percorso non è una cartella."));
}

void symlinkedFoldersAreListedButBrokenOnesAreNot() {
    TempDir dir;
    fs::create_directories(dir.path() / "real");
    fs::create_directory_symlink(dir.path() / "real", dir.path() / "link");
    fs::create_directory_symlink(dir.path() / "missing", dir.path() / "dangling");
    makeFile(dir.path() / "f", 1);
    fs::create_symlink(dir.path() / "f", dir.path() / "fileLink");
    const auto listing = listFolders(dir.path());
    CHECK(names(listing) == (std::vector<std::string>{"link", "real"}));
}

void limitTruncatesInsteadOfFailing() {
    TempDir dir;
    for (int i = 0; i < 30; ++i) fs::create_directories(dir.path() / ("d" + std::to_string(i)));
    const auto listing = listFolders(dir.path(), 10);
    CHECK(listing.found);
    CHECK(listing.truncated);
    CHECK_EQ(listing.folders.size(), std::size_t{10});
}

void invalidUtf8NamesAreRepaired() {
    TempDir dir;
    fs::create_directories(dir.path() / "bad\xFF" "name");
    const auto listing = listFolders(dir.path());
    CHECK_EQ(listing.folders.size(), std::size_t{1});
    if (!listing.folders.empty()) CHECK_EQ(listing.folders[0].name, std::string("bad\xEF\xBF\xBD" "name"));
}

void unreadableDirectoryIsReported() {
    if (::geteuid() == 0) return; // root ignores permissions: nothing to test
    TempDir dir;
    fs::create_directories(dir.path() / "locked");
    fs::permissions(dir.path() / "locked", fs::perms::none);
    const auto listing = listFolders(dir.path() / "locked");
    CHECK(!listing.found);
    CHECK(!listing.error.empty());
}

void shortcutsExistAndAreAbsolute() {
    const auto shortcuts = folderShortcuts();
    CHECK(!shortcuts.empty());
    for (const auto& s : shortcuts) {
        CHECK(!s.name.empty());
        CHECK(pathFromUtf8(s.path).is_absolute());
        CHECK(fs::is_directory(pathFromUtf8(s.path)));
    }
}

void nothingIsModified() {
    TempDir dir;
    fs::create_directories(dir.path() / "a");
    makeFile(dir.path() / "a" / "f", 3);
    (void)listFolders(dir.path());
    (void)listFolders(dir.path() / "a");
    CHECK(fs::exists(dir.path() / "a" / "f"));
    CHECK_EQ(fs::file_size(dir.path() / "a" / "f"), std::uintmax_t{3});
}

int main() {
    RUN(listsOnlyFoldersSortedWithoutCase);
    RUN(parentNavigation);
    RUN(trailingSeparatorsAndDotsAreNormalised);
    RUN(emptyPathMeansTheTopOfTheFileSystem);
    RUN(missingAndNonDirectoryPathsAreReported);
    RUN(symlinkedFoldersAreListedButBrokenOnesAreNot);
    RUN(limitTruncatesInsteadOfFailing);
    RUN(invalidUtf8NamesAreRepaired);
    RUN(unreadableDirectoryIsReported);
    RUN(shortcutsExistAndAreAbsolute);
    RUN(nothingIsModified);
    return testReport("folder_tests");
}
