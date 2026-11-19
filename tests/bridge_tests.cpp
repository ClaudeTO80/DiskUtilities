/**
 * @file bridge_tests.cpp
 * @brief Tests for SnapshotBridge and its DTO mapping.
 * @details Covers unscanned roots, paging, sorting, filtering and timestamp formatting.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/filesystem/DirectoryScanner.h"
#include "diskutilities/web/SnapshotBridge.h"

#include <atomic>
#include <thread>

using namespace diskutilities;
using namespace diskutilities::web;
namespace fs = std::filesystem;

static void publishScan(SnapshotBridge& bridge, const fs::path& root) {
    auto tree = DirectoryScanner{}.scan(root);
    CHECK(tree.has_value());
    if (tree) bridge.publish(Snapshot::create(std::move(*tree), VolumeInfo{1000, 400, 300, true}, std::chrono::milliseconds(42)));
}

static void sampleTree(const fs::path& root) {
    makeFile(root / "docs" / "a.txt", 100);
    makeFile(root / "docs" / "b.txt", 300);
    makeFile(root / "media" / "v.mp4", 5000);
    makeFile(root / "media" / "photos" / "p.jpg", 700);
    fs::create_directories(root / "empty");
    makeFile(root / "readme", 9);
}

void timestampFormatting() {
    CHECK_EQ(SnapshotBridge::formatTimestamp(0), std::string("1970-01-01T00:00:00Z"));
    CHECK_EQ(SnapshotBridge::formatTimestamp(1'700'000'000), std::string("2023-11-14T22:13:20Z"));
    CHECK_EQ(SnapshotBridge::formatTimestamp(-1), std::string("1969-12-31T23:59:59Z"));
    CHECK_EQ(SnapshotBridge::formatTimestamp(951'782'400), std::string("2000-02-29T00:00:00Z"));
    CHECK_EQ(SnapshotBridge::formatTimestamp(4'102'444'799), std::string("2099-12-31T23:59:59Z"));
    CHECK_EQ(SnapshotBridge::formatTimestamp(kNoTimestamp), std::string());
}

void beforeFirstScanEverythingIsEmpty() {
    SnapshotBridge bridge;
    const auto dash = bridge.getDashboard("/anything");
    CHECK(!dash.scanned);
    CHECK_EQ(dash.fileCount, std::uint64_t{0});
    CHECK(bridge.getLargestFiles("/anything", 10).empty());
    CHECK(bridge.getExtensionStatistics("/anything").empty());
    CHECK(!bridge.listDirectory("/anything", {}).found);
    CHECK(bridge.getDirectoryTree("/anything", 3).children.empty());
}

void dashboardReflectsSnapshot() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());
    const auto dash = bridge.getDashboard(dir.path());
    CHECK(dash.scanned);
    CHECK_EQ(dash.totalBytes, std::uint64_t{100 + 300 + 5000 + 700 + 9});
    CHECK_EQ(dash.fileCount, std::uint64_t{5});
    CHECK_EQ(dash.directoryCount, std::uint64_t{5}); // root, docs, media, photos, empty
    CHECK_EQ(dash.scanDurationMs, std::uint64_t{42});
    CHECK(dash.volume.valid);
    CHECK_EQ(dash.volume.capacity, std::uint64_t{1000});
    CHECK_EQ(dash.largestDirectories.size(), std::size_t{3});
    CHECK_EQ(dash.largestDirectories[0].name, std::string("media"));
    CHECK_EQ(dash.largestDirectories[0].size, std::uint64_t{5700});
    // A trailing slash and "." segments still address the same root.
    CHECK(bridge.getDashboard(dir.path().string() + "/").scanned);
    CHECK(bridge.getDashboard(dir.path() / "docs" / "..").scanned);
    // A different root is "not scanned" rather than silently wrong data.
    CHECK(!bridge.getDashboard(dir.path() / "docs").scanned);
}

void largestFilesAndExtensions() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());
    const auto files = bridge.getLargestFiles(dir.path(), 3);
    CHECK_EQ(files.size(), std::size_t{3});
    CHECK_EQ(files[0].name, std::string("v.mp4"));
    CHECK_EQ(files[0].extension, std::string(".mp4"));
    CHECK_EQ(files[0].path, (dir.path() / "media" / "v.mp4").generic_string());
    CHECK_EQ(files[1].name, std::string("p.jpg"));
    CHECK(!files[0].modified.empty());
    const auto exts = bridge.getExtensionStatistics(dir.path());
    CHECK_EQ(exts[0].extension, std::string(".mp4"));
    CHECK_EQ(exts.size(), std::size_t{4}); // .mp4 .jpg .txt senza estensione
}

void listingAndTree() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());

    auto root = bridge.listDirectory(dir.path(), {});
    CHECK(root.found);
    CHECK_EQ(root.total, std::size_t{4});
    CHECK_EQ(root.entries.size(), std::size_t{4});
    CHECK_EQ(root.entries[0].name, std::string("docs"));
    CHECK_EQ(root.entries[0].type, std::string("directory"));
    CHECK_EQ(root.entries[0].size, std::uint64_t{400});
    CHECK_EQ(root.entries[0].fileCount, std::uint64_t{2});
    CHECK(!root.entries[0].hasSubdirectories);
    CHECK(root.entries[2].hasSubdirectories || root.entries[1].hasSubdirectories);
    CHECK(!root.truncated);

    ListOptions page;
    page.limit = 2;
    auto first = bridge.listDirectory(dir.path(), page);
    CHECK(first.truncated);
    CHECK_EQ(first.entries.size(), std::size_t{2});
    page.offset = 2;
    auto second = bridge.listDirectory(dir.path(), page);
    CHECK(!second.truncated);

    // Listing a sub-directory by the path the UI received from the previous call.
    auto docs = bridge.listDirectory(root.entries[0].path, {});
    CHECK(docs.found);
    CHECK_EQ(docs.entries.size(), std::size_t{2});
    CHECK(!bridge.listDirectory(dir.path() / "nope", {}).found);
    CHECK(!bridge.listDirectory(dir.path() / "readme", {}).found); // a file is not a directory

    const auto tree = bridge.getDirectoryTree(dir.path(), 1);
    CHECK_EQ(tree.children.size(), std::size_t{3});
    for (const auto& child : tree.children) CHECK(child.children.empty()); // depth 1 stops here
    const auto deep = bridge.getDirectoryTree(dir.path(), 5);
    std::size_t photos = 0;
    for (const auto& child : deep.children)
        if (child.name == "media") photos = child.children.size();
    CHECK_EQ(photos, std::size_t{1});
    CHECK_EQ(bridge.getDirectoryTree(dir.path(), 0).children.size(), std::size_t{0});
}

void queriesAreSafeWhileRepublishing() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            while (!stop) {
                const auto d = bridge.getDashboard(dir.path());
                if (d.scanned && d.fileCount != 5) ++bad;
                const auto l = bridge.listDirectory(dir.path(), {});
                if (l.found && l.total != 4) ++bad;
            }
        });
    for (int i = 0; i < 30; ++i) publishScan(bridge, dir.path());
    stop = true;
    for (auto& t : readers) t.join();
    CHECK_EQ(bad.load(), 0);
}

void largestFilesInDirectory() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());

    // Files below "media" at any depth: v.mp4 (5000) and photos/p.jpg (700).
    const auto media = bridge.getLargestFilesIn(dir.path() / "media", 10);
    CHECK_EQ(media.size(), std::size_t{2});
    CHECK_EQ(media[0].name, std::string("v.mp4"));
    CHECK_EQ(media[1].path, (dir.path() / "media" / "photos" / "p.jpg").generic_string());
    CHECK_EQ(bridge.getLargestFilesIn(dir.path() / "media", 1).size(), std::size_t{1});
    CHECK_EQ(bridge.getLargestFilesIn(dir.path() / "media" / "photos", 10)[0].name, std::string("p.jpg"));
    // The root covers everything, the same ranking as the whole-tree query.
    CHECK_EQ(bridge.getLargestFilesIn(dir.path(), 10).size(), std::size_t{5});
    // Empty directory, file, unknown path and an unscanned root all give nothing.
    CHECK(bridge.getLargestFilesIn(dir.path() / "empty", 10).empty());
    CHECK(bridge.getLargestFilesIn(dir.path() / "readme", 10).empty());
    CHECK(bridge.getLargestFilesIn(dir.path() / "nope", 10).empty());
    CHECK(bridge.getLargestFilesIn(dir.path() / "media", 0).empty());
    SnapshotBridge unscanned;
    CHECK(unscanned.getLargestFilesIn(dir.path(), 10).empty());
}

void largestFilesInMatchesGlobalRanking() {
    // Many equal sizes make the tie-break visible: the subtree answer must be the global order restricted to it.
    TempDir dir;
    for (int i = 0; i < 60; ++i) {
        const fs::path sub = dir.path() / ("d" + std::to_string(i % 5)) / ("s" + std::to_string(i % 3));
        makeFile(sub / ("f" + std::to_string(i) + ".bin"), static_cast<std::uintmax_t>(100 * (i % 7 + 1)));
    }
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());
    const auto everything = bridge.getLargestFiles(dir.path(), 10000);
    CHECK_EQ(everything.size(), std::size_t{60});
    for (const std::string sub : {"d0", "d3", "d4/s1", "d2/s2"}) {
        const std::string prefix = (dir.path() / sub).generic_string() + "/";
        std::vector<std::string> expected;
        for (const auto& file : everything)
            if (file.path.rfind(prefix, 0) == 0) expected.push_back(file.path);
        std::vector<std::string> actual;
        for (const auto& file : bridge.getLargestFilesIn(dir.path() / sub, 10000)) actual.push_back(file.path);
        CHECK(!expected.empty());
        CHECK(expected == actual);
        const auto top3 = bridge.getLargestFilesIn(dir.path() / sub, 3);
        CHECK_EQ(top3.size(), std::size_t{3});
        for (std::size_t i = 0; i < top3.size(); ++i) CHECK(top3[i].path == expected[i]);
    }
}

void treemapBlocks() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());

    const auto all = bridge.getTreemap(dir.path(), 10, 1);
    CHECK(all.found);
    CHECK_EQ(all.totalBytes, std::uint64_t{100 + 300 + 5000 + 700 + 9});
    CHECK_EQ(all.childCount, std::size_t{4}); // docs, media, empty, readme
    CHECK_EQ(all.items.size(), std::size_t{3}); // "empty" has no area and is not drawn
    CHECK_EQ(all.items[0].name, std::string("media"));
    CHECK(all.items[0].directory);
    CHECK_EQ(all.items[0].size, std::uint64_t{5700});
    CHECK_EQ(all.items[1].name, std::string("docs"));
    CHECK_EQ(all.items[2].name, std::string("readme"));
    CHECK(!all.items[2].directory);
    for (const auto& item : all.items) CHECK(!item.aggregate);

    // Only two blocks fit: the biggest child plus one aggregate that folds the other two.
    const auto capped = bridge.getTreemap(dir.path(), 2, 1);
    CHECK_EQ(capped.items.size(), std::size_t{2});
    CHECK_EQ(capped.items[0].name, std::string("media"));
    CHECK(capped.items[1].aggregate);
    CHECK_EQ(capped.items[1].count, std::size_t{2});
    CHECK_EQ(capped.items[1].size, std::uint64_t{400 + 9});
    CHECK(capped.items[1].path.empty());

    const auto onlyRest = bridge.getTreemap(dir.path(), 0, 1);
    CHECK_EQ(onlyRest.items.size(), std::size_t{1});
    CHECK(onlyRest.items[0].aggregate);
    CHECK_EQ(onlyRest.items[0].count, std::size_t{3});
    CHECK_EQ(onlyRest.items[0].size, std::uint64_t{6109});

    CHECK(!bridge.getTreemap(dir.path() / "readme", 5, 1).found);
    CHECK(!bridge.getTreemap(dir.path() / "nope", 5, 1).found);
    CHECK(!SnapshotBridge{}.getTreemap(dir.path(), 5, 1).found);
}

void treemapNestsSubfolders() {
    TempDir dir;
    sampleTree(dir.path());
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());

    // depth 1: direct children only, no nested blocks.
    const auto flat = bridge.getTreemap(dir.path(), 10, 1);
    for (const auto& item : flat.items) CHECK(item.children.empty());

    // depth 2: media shows v.mp4 and photos; photos is not expanded yet.
    const auto two = bridge.getTreemap(dir.path(), 10, 2);
    const auto& media = two.items[0];
    CHECK_EQ(media.name, std::string("media"));
    CHECK_EQ(media.children.size(), std::size_t{2});
    CHECK_EQ(media.children[0].name, std::string("v.mp4"));
    CHECK_EQ(media.children[1].name, std::string("photos"));
    CHECK(media.children[1].children.empty());
    CHECK(two.items[2].children.empty()); // readme is a file

    // depth 3: photos is expanded too.
    const auto three = bridge.getTreemap(dir.path(), 10, 3);
    const auto& photos = three.items[0].children[1];
    CHECK_EQ(photos.children.size(), std::size_t{1});
    CHECK_EQ(photos.children[0].name, std::string("p.jpg"));

    // A single block per level: only the aggregate remains, so nothing is nested.
    const auto capped = bridge.getTreemap(dir.path(), 1, 3);
    CHECK_EQ(capped.items.size(), std::size_t{1});
    CHECK(capped.items[0].aggregate);
    CHECK(capped.items[0].children.empty());
}

void treemapBudgetBoundsBlocks() {
    // 60 folders of 60 files: far more blocks than the budget, so the total must stay within it.
    TempDir dir;
    for (int d = 0; d < 60; ++d)
        for (int f = 0; f < 60; ++f)
            makeFile(dir.path() / ("d" + std::to_string(d)) / ("s" + std::to_string(f % 4)) / ("f" + std::to_string(f) + ".bin"),
                     static_cast<std::uintmax_t>(10 + d * 60 + f));
    SnapshotBridge bridge;
    publishScan(bridge, dir.path());
    const auto dto = bridge.getTreemap(dir.path(), 100, 3);
    std::size_t blocks = 0;
    std::vector<const TreemapItemDto*> pending;
    for (const auto& item : dto.items) pending.push_back(&item);
    while (!pending.empty()) {
        const TreemapItemDto* item = pending.back();
        pending.pop_back();
        ++blocks;
        for (const auto& child : item->children) pending.push_back(&child);
    }
    CHECK(blocks > 60);
    CHECK(blocks <= 2500);
}

int main() {
    RUN(timestampFormatting);
    RUN(beforeFirstScanEverythingIsEmpty);
    RUN(dashboardReflectsSnapshot);
    RUN(largestFilesAndExtensions);
    RUN(listingAndTree);
    RUN(largestFilesInDirectory);
    RUN(largestFilesInMatchesGlobalRanking);
    RUN(treemapBlocks);
    RUN(treemapNestsSubfolders);
    RUN(treemapBudgetBoundsBlocks);
    RUN(queriesAreSafeWhileRepublishing);
    return testReport("bridge_tests");
}
