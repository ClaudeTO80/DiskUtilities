/**
 * @file snapshot_tests.cpp
 * @brief Tests for Snapshot.
 * @details Covers aggregation, ordering, tie-breaking, paging and invalid trees.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/analysis/Snapshot.h"

#include <algorithm>
#include <stdexcept>

using namespace diskutilities;
namespace fs = std::filesystem;

namespace {

struct Builder {
    FileTree tree;
    explicit Builder(fs::path root = "/data") {
        tree.root = std::move(root);
        Node r;
        r.name = "data";
        r.type = EntryType::Directory;
        tree.nodes.push_back(r);
    }
    std::uint32_t add(std::uint32_t parent, std::string name, EntryType type, std::uint64_t size = 0,
                      std::int64_t modified = kNoTimestamp, bool accessible = true) {
        Node n;
        n.name = std::move(name);
        n.type = type;
        n.size = size;
        n.modified = modified;
        n.parent = parent;
        n.accessible = accessible;
        tree.nodes.push_back(std::move(n));
        return static_cast<std::uint32_t>(tree.nodes.size() - 1);
    }
    std::uint32_t dir(std::uint32_t parent, std::string name) { return add(parent, std::move(name), EntryType::Directory); }
    std::uint32_t file(std::uint32_t parent, std::string name, std::uint64_t size, std::int64_t modified = kNoTimestamp) {
        return add(parent, std::move(name), EntryType::RegularFile, size, modified);
    }
};

/*  /data
 *    docs/            (2 files: a.TXT 100, b.txt 300)
 *    media/           (video.mp4 5000, photos/ {p1.jpg 700, p2.JPG 800})
 *    empty/
 *    noext 40, .bashrc 7, trailing. 3, link -> (symlink)
 */
struct Sample {
    Builder b;
    std::uint32_t docs, media, photos, empty, noext, bashrc, trailing, link;
    Sample() {
        docs = b.dir(0, "docs");
        b.file(docs, "b.txt", 300, 2000);
        b.file(docs, "a.TXT", 100, 1000);
        media = b.dir(0, "media");
        b.file(media, "video.mp4", 5000, 3000);
        photos = b.dir(media, "photos");
        b.file(photos, "p1.jpg", 700);
        b.file(photos, "p2.JPG", 800);
        empty = b.dir(0, "empty");
        noext = b.file(0, "noext", 40);
        bashrc = b.file(0, ".bashrc", 7);
        trailing = b.file(0, "trailing.", 3);
        link = b.add(0, "link", EntryType::Symlink);
    }
    std::shared_ptr<const Snapshot> snap(VolumeInfo v = {}) { return Snapshot::create(b.tree, v); }
};

} // namespace

void aggregatesAndTotals() {
    Sample s;
    const auto snap = s.snap();
    CHECK_EQ(snap->bytesOf(0), std::uint64_t{100 + 300 + 5000 + 700 + 800 + 40 + 7 + 3});
    CHECK_EQ(snap->bytesOf(s.docs), std::uint64_t{400});
    CHECK_EQ(snap->bytesOf(s.media), std::uint64_t{6500});
    CHECK_EQ(snap->bytesOf(s.photos), std::uint64_t{1500});
    CHECK_EQ(snap->bytesOf(s.empty), std::uint64_t{0});
    CHECK_EQ(snap->filesOf(0), std::uint64_t{8});
    CHECK_EQ(snap->filesOf(s.media), std::uint64_t{3});
    CHECK_EQ(snap->totals().files, std::uint64_t{8});
    CHECK_EQ(snap->totals().directories, std::uint64_t{5}); // root, docs, media, photos, empty
    CHECK_EQ(snap->totals().symlinks, std::uint64_t{1});
    CHECK_EQ(snap->totals().bytes, snap->bytesOf(0));
    CHECK_EQ(snap->nodeCount(), s.b.tree.nodes.size());
}

void inaccessibleAreCounted() {
    Sample s;
    s.b.add(s.empty, "locked", EntryType::Directory, 0, kNoTimestamp, false);
    s.b.tree.nodes[s.docs].accessible = false;
    CHECK_EQ(s.snap()->totals().inaccessible, std::uint64_t{2});
}

void childrenAreSortedByName() {
    Sample s;
    const auto snap = s.snap();
    std::vector<std::string> names;
    for (const auto id : snap->children(0)) names.push_back(snap->node(id).name);
    CHECK(std::is_sorted(names.begin(), names.end()));
    CHECK_EQ(names.size(), std::size_t{7});
    CHECK(snap->children(s.empty).empty());
    CHECK(snap->hasSubdirectories(0));
    CHECK(snap->hasSubdirectories(s.media));
    CHECK(!snap->hasSubdirectories(s.docs));
    CHECK(!snap->hasSubdirectories(s.empty));
}

void pathsAndLookup() {
    Sample s;
    const auto snap = s.snap();
    CHECK_EQ(snap->rootUtf8(), std::string("/data"));
    CHECK_EQ(snap->pathOf(0), std::string("/data"));
    CHECK_EQ(snap->pathOf(s.photos), std::string("/data/media/photos"));
    CHECK(snap->find("/data").value_or(99) == 0);
    CHECK(snap->find("/data/").value_or(99) == 0);
    CHECK(snap->find("/data/media/photos").value_or(99) == s.photos);
    CHECK(snap->find("/data/media/../media/photos/./").value_or(99) == s.photos);
    CHECK(!snap->find("/data/media/nope").has_value());
    CHECK(!snap->find("/other").has_value());
    CHECK(!snap->find("/").has_value());
    CHECK(!snap->find("/data/..").has_value());
    // find() must agree with pathOf() for every node.
    for (NodeId id = 0; id < snap->nodeCount(); ++id) CHECK(snap->find(snap->pathOf(id)).value_or(999) == id);
}

void rootVariants() {
    Builder b("/");
    b.file(0, "f", 1);
    const auto snap = Snapshot::create(b.tree);
    CHECK_EQ(snap->pathOf(1), std::string("/f"));
    CHECK(snap->find("/f").value_or(99) == 1);

    Builder trailing("/data/");
    trailing.file(0, "f", 1);
    const auto snap2 = Snapshot::create(trailing.tree);
    CHECK_EQ(snap2->rootUtf8(), std::string("/data"));
    CHECK_EQ(snap2->pathOf(1), std::string("/data/f"));
}

void extensionStatistics() {
    Sample s;
    const auto snap = s.snap();
    const auto& stats = snap->extensions();
    const auto find = [&](std::string_view ext) -> const ExtensionStat* {
        for (const auto& st : stats) if (st.extension == ext) return &st;
        return nullptr;
    };
    CHECK(find(".mp4") && find(".mp4")->bytes == 5000 && find(".mp4")->count == 1);
    CHECK(find(".jpg") && find(".jpg")->count == 2 && find(".jpg")->bytes == 1500); // case-folded
    CHECK(find(".txt") && find(".txt")->count == 2 && find(".txt")->bytes == 400);
    // "noext", ".bashrc" and "trailing." have no usable extension.
    const auto* none = find(Snapshot::kNoExtension);
    CHECK(none && none->count == 3 && none->bytes == 50);
    CHECK_EQ(stats.size(), std::size_t{4});
    CHECK(std::is_sorted(stats.begin(), stats.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; }));
    CHECK_EQ(snap->extensionOf(s.noext), std::string(Snapshot::kNoExtension));
    CHECK_EQ(snap->extensionOf(s.docs), std::string()); // directories have none
}

void largestFilesAreOrderedAndDeterministic() {
    Builder b;
    const auto d = b.dir(0, "d");
    // Many equal sizes: the order must come from path order, not from insertion order.
    const auto z = b.file(d, "z", 10);
    const auto a = b.file(d, "a", 10);
    const auto top = b.file(0, "top", 99);
    const auto m = b.file(0, "m", 10);
    const auto snap = Snapshot::create(b.tree);
    const auto largest = snap->largestFiles(10);
    CHECK_EQ(largest.size(), std::size_t{4});
    CHECK_EQ(largest[0], top);
    // Path order: "/data/d/a" < "/data/d/z" < "/data/m"
    CHECK_EQ(largest[1], a);
    CHECK_EQ(largest[2], z);
    CHECK_EQ(largest[3], m);
    CHECK_EQ(snap->largestFiles(2).size(), std::size_t{2});
    CHECK_EQ(snap->largestFiles(0).size(), std::size_t{0});
}

void largestFilesHonoursCap() {
    Builder b;
    for (std::size_t i = 0; i < Snapshot::kMaxLargestFiles + 50; ++i) b.file(0, "f" + std::to_string(i), i);
    const auto snap = Snapshot::create(b.tree);
    CHECK_EQ(snap->largestFiles(1'000'000).size(), Snapshot::kMaxLargestFiles);
    CHECK_EQ(snap->node(snap->largestFiles(1)[0]).size, std::uint64_t{Snapshot::kMaxLargestFiles + 49});
}

void topLevelDirectories() {
    Sample s;
    const auto top = s.snap()->largestTopLevelDirectories(10);
    CHECK_EQ(top.size(), std::size_t{3});
    CHECK_EQ(top[0], s.media);
    CHECK_EQ(top[1], s.docs);
    CHECK_EQ(top[2], s.empty);
    CHECK_EQ(s.snap()->largestTopLevelDirectories(1).size(), std::size_t{1});
}

void listingSortsFiltersAndPages() {
    Sample s;
    const auto snap = s.snap();
    const auto names = [&](const Listing& l) {
        std::vector<std::string> out;
        for (const auto id : l.entries) out.push_back(snap->node(id).name);
        return out;
    };

    ListQuery q;
    auto byName = snap->list(0, q);
    CHECK_EQ(byName.total, std::size_t{7});
    // Directories first (case-insensitive name order), then files.
    CHECK_EQ(names(byName)[0], std::string("docs"));
    CHECK_EQ(names(byName)[1], std::string("empty"));
    CHECK_EQ(names(byName)[2], std::string("media"));
    CHECK_EQ(names(byName)[3], std::string(".bashrc"));

    q.sort = SortKey::Size;
    q.descending = true;
    auto bySize = snap->list(0, q);
    CHECK_EQ(names(bySize)[0], std::string("media")); // 6500
    CHECK_EQ(names(bySize)[1], std::string("docs"));  // 400
    CHECK_EQ(names(bySize)[2], std::string("empty")); // 0
    CHECK_EQ(names(bySize)[3], std::string("noext")); // biggest file: 40

    q = {};
    q.sort = SortKey::Modified;
    q.descending = true;
    auto docs = snap->list(s.docs, q);
    CHECK_EQ(names(docs)[0], std::string("b.txt")); // modified 2000 > 1000

    q = {};
    q.sort = SortKey::Type;
    auto photos = snap->list(s.photos, q);
    CHECK_EQ(photos.total, std::size_t{2});

    q = {};
    q.filter = "TXT"; // case-insensitive
    CHECK_EQ(snap->list(s.docs, q).total, std::size_t{2});
    q.filter = "a.t";
    CHECK_EQ(names(snap->list(s.docs, q)), std::vector<std::string>{"a.TXT"});
    q.filter = "does-not-match";
    CHECK_EQ(snap->list(0, q).total, std::size_t{0});

    q = {};
    q.directoriesOnly = true;
    CHECK_EQ(snap->list(0, q).total, std::size_t{3});

    // Paging: 7 entries, pages of 3.
    q = {};
    q.limit = 3;
    const auto page0 = snap->list(0, q);
    q.offset = 3;
    const auto page1 = snap->list(0, q);
    q.offset = 6;
    const auto page2 = snap->list(0, q);
    q.offset = 9;
    const auto beyond = snap->list(0, q);
    CHECK_EQ(page0.entries.size(), std::size_t{3});
    CHECK_EQ(page1.entries.size(), std::size_t{3});
    CHECK_EQ(page2.entries.size(), std::size_t{1});
    CHECK_EQ(beyond.entries.size(), std::size_t{0});
    CHECK_EQ(page2.total, std::size_t{7});
    std::vector<NodeId> all;
    for (const auto* p : {&page0, &page1, &page2}) all.insert(all.end(), p->entries.begin(), p->entries.end());
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end()); // no duplicates across pages
    CHECK_EQ(all.size(), std::size_t{7});

    // Total bytes reflects the filter, not the page.
    q = {};
    q.limit = 1;
    CHECK_EQ(snap->list(0, q).totalBytes, snap->bytesOf(0));
}

void pagedSortMatchesFullSort() {
    Builder b;
    for (int i = 0; i < 500; ++i) b.file(0, "f" + std::to_string((i * 37) % 500), static_cast<std::uint64_t>((i * 13) % 50));
    const auto snap = Snapshot::create(b.tree);
    ListQuery full;
    full.sort = SortKey::Size;
    full.descending = true;
    full.limit = 500;
    const auto all = snap->list(0, full).entries;
    ListQuery page = full;
    page.limit = 20;
    page.offset = 100;
    const auto part = snap->list(0, page).entries;
    CHECK_EQ(part.size(), std::size_t{20});
    CHECK(std::equal(part.begin(), part.end(), all.begin() + 100));
}

void volumeAndDurationAreKept() {
    Sample s;
    const auto snap = Snapshot::create(s.b.tree, VolumeInfo{1000, 400, 300, true}, std::chrono::milliseconds(1234));
    CHECK(snap->volume().valid);
    CHECK_EQ(snap->volume().capacity, std::uint64_t{1000});
    CHECK_EQ(snap->volume().available, std::uint64_t{300});
    CHECK_EQ(snap->scanDuration().count(), 1234);
}

void invalidTreesAreRejected() {
    const auto throws = [](FileTree tree) {
        try { (void)Snapshot::create(std::move(tree)); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    CHECK(throws(FileTree{}));
    {
        Sample s;
        s.b.tree.nodes[0].parent = 0; // root must have no parent
        CHECK(throws(s.b.tree));
    }
    {
        Sample s;
        s.b.tree.nodes[2].parent = 5; // parent after child
        CHECK(throws(s.b.tree));
    }
    {
        Sample s;
        s.b.tree.nodes[0].type = EntryType::RegularFile;
        CHECK(throws(s.b.tree));
    }
}

void singleNodeTree() {
    Builder b;
    const auto snap = Snapshot::create(b.tree);
    CHECK_EQ(snap->nodeCount(), std::size_t{1});
    CHECK(snap->children(0).empty());
    CHECK_EQ(snap->totals().directories, std::uint64_t{1});
    CHECK(snap->largestFiles(10).empty());
    CHECK(snap->extensions().empty());
    CHECK_EQ(snap->list(0, {}).total, std::size_t{0});
}

int main() {
    RUN(aggregatesAndTotals);
    RUN(inaccessibleAreCounted);
    RUN(childrenAreSortedByName);
    RUN(pathsAndLookup);
    RUN(rootVariants);
    RUN(extensionStatistics);
    RUN(largestFilesAreOrderedAndDeterministic);
    RUN(largestFilesHonoursCap);
    RUN(topLevelDirectories);
    RUN(listingSortsFiltersAndPages);
    RUN(pagedSortMatchesFullSort);
    RUN(volumeAndDurationAreKept);
    RUN(invalidTreesAreRejected);
    RUN(singleNodeTree);
    return testReport("snapshot_tests");
}
