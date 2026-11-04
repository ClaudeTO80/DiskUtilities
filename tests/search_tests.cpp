/**
 * @file search_tests.cpp
 * @brief Tests for FileSearcher.
 * @details Covers name, pattern, size, modification and creation filters, entry kinds, hidden entries, symlinks,
 *          result cap, cancellation, validation, live hits, progress, external executors, invalid UTF-8 names
 *          and unreadable directories.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/filesystem/EntryStat.h"
#include "diskutilities/filesystem/FileSearcher.h"
#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace diskutilities;
namespace fs = std::filesystem;

namespace {

/** Set the modification time of @p path to @p seconds since the epoch (POSIX utimensat). */
void setMtime(const fs::path& path, std::int64_t seconds) {
    struct timespec times[2];
    times[0].tv_sec = 0; times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(seconds); times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, path.c_str(), times, 0);
}

std::vector<std::string> relNames(const SearchResult& r, const fs::path& root) {
    std::vector<std::string> out;
    const std::string prefix = pathToUtf8(root) + "/";
    for (const auto& h : r.hits) out.push_back(h.path.substr(prefix.size()));
    std::sort(out.begin(), out.end());
    return out;
}

using Names = std::vector<std::string>;

/** Search with default options and abort the test body (via CHECK) if it fails. */
SearchResult run(const fs::path& root, const SearchCriteria& c, SearchOptions o = {}) {
    o.threads = 3;
    auto result = FileSearcher{}.search(root, c, o);
    CHECK(result.has_value());
    return result ? std::move(*result) : SearchResult{};
}

/** root/ a.txt(10) B.TXT(2000) sub/c.cpp(500) sub/deep/d.TXT(70000) .hidden/e.txt(5) empty/ */
void makeSample(const fs::path& root) {
    makeFile(root / "a.txt", 10);
    makeFile(root / "B.TXT", 2000);
    makeFile(root / "sub" / "c.cpp", 500);
    makeFile(root / "sub" / "deep" / "d.TXT", 70000);
    makeFile(root / ".hidden" / "e.txt", 5);
    fs::create_directories(root / "empty");
}

} // namespace

void everythingWithoutCriteria() {
    TempDir dir;
    makeSample(dir.path());
    const auto r = run(dir.path(), {});
    CHECK(relNames(r, dir.path()) == (Names{".hidden/e.txt", "B.TXT", "a.txt", "sub/c.cpp", "sub/deep/d.TXT"}));
    CHECK(!r.truncated);
    CHECK_EQ(r.directories, std::uint64_t{5}); // root, sub, deep, .hidden, empty
    CHECK_EQ(r.hits.size(), std::size_t{5});
}

void nameSubstringIsCaseInsensitiveByDefault() {
    TempDir dir;
    makeSample(dir.path());
    SearchCriteria c;
    c.nameContains = "txt";
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{".hidden/e.txt", "B.TXT", "a.txt", "sub/deep/d.TXT"}));
    c.caseSensitive = true;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{".hidden/e.txt", "a.txt"}));
}

void patternsAreOrCombinedAndExcludesWin() {
    TempDir dir;
    makeSample(dir.path());
    SearchCriteria c;
    c.patterns = {"*.cpp", "a.*"};
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"a.txt", "sub/c.cpp"}));
    c.patterns = {"*.txt"};
    c.excludePatterns = {"a*"};
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{".hidden/e.txt", "B.TXT", "sub/deep/d.TXT"}));
    c.nameContains = "d.";           // AND with the substring
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"sub/deep/d.TXT"}));
}

void sizeBoundsAreInclusive() {
    TempDir dir;
    makeSample(dir.path());
    SearchCriteria c;
    c.minSize = 500;
    c.maxSize = 2000;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"B.TXT", "sub/c.cpp"}));
    c.minSize = 2001;
    c.maxSize.reset();
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"sub/deep/d.TXT"}));
    c.minSize.reset();
    c.maxSize = 9;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{".hidden/e.txt"}));
    const auto r = run(dir.path(), c);
    CHECK_EQ(r.hits.at(0).size, std::uint64_t{5});
}

void modificationDateRange() {
    TempDir dir;
    makeSample(dir.path());
    setMtime(dir.path() / "a.txt", 1'000'000);
    setMtime(dir.path() / "B.TXT", 2'000'000);
    setMtime(dir.path() / "sub" / "c.cpp", 3'000'000);
    SearchCriteria c;
    c.modifiedAfter = 2'000'000;
    c.modifiedBefore = 3'000'000;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"B.TXT", "sub/c.cpp"}));
    c.modifiedAfter = 2'000'001;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"sub/c.cpp"}));
    c.modifiedAfter.reset();
    c.modifiedBefore = 1'000'000;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"a.txt"}));
    CHECK_EQ(run(dir.path(), c).hits.at(0).modified, std::int64_t{1'000'000});
}

void creationDateRange() {
    TempDir dir;
    makeSample(dir.path());
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    // Probe whether this filesystem records a birth time at all.
    const auto probe = detail::probeEntry(fs::directory_entry(dir.path() / "a.txt"), true);
    SearchCriteria c;
    c.createdAfter = now - 3600;
    c.createdBefore = now + 3600;
    const auto all = run(dir.path(), c);
    if (probe.created == kNoTimestamp) {
        std::cerr << "  note: no birth time on this platform/filesystem: checking the graceful path only\n";
        CHECK(all.hits.empty());
        CHECK_EQ(all.creationUnavailable, std::uint64_t{5});
        return;
    }
    CHECK_EQ(all.hits.size(), std::size_t{5});
    CHECK_EQ(all.creationUnavailable, std::uint64_t{0});
    CHECK(std::llabs(all.hits.at(0).created - now) < 3600);
    c.createdAfter = now + 3600;
    c.createdBefore.reset();
    CHECK(run(dir.path(), c).hits.empty());
    c.createdAfter.reset();
    c.createdBefore = now - 3600;
    CHECK(run(dir.path(), c).hits.empty());
    // A modification-time change must not alter the creation time (this is what separates btime from ctime).
    setMtime(dir.path() / "a.txt", 1'000);
    c.createdBefore.reset();
    c.createdAfter = now - 3600;
    CHECK_EQ(run(dir.path(), c).hits.size(), std::size_t{5});
}

void creationIsReportedOnlyWhenAsked() {
    TempDir dir;
    makeSample(dir.path());
    const auto probe = detail::probeEntry(fs::directory_entry(dir.path() / "a.txt"), true);
    if (probe.created == kNoTimestamp) { std::cerr << "  note: no birth time here: skipped\n"; return; }
    SearchOptions o;
    CHECK_EQ(run(dir.path(), {}, o).hits.at(0).created, kNoTimestamp); // not asked: no extra syscall
    o.reportCreation = true;
    const auto r = run(dir.path(), {}, o);
    for (const auto& h : r.hits) CHECK(h.created != kNoTimestamp);
}

void kindsAndDirectories() {
    TempDir dir;
    makeSample(dir.path());
    SearchCriteria c;
    c.kind = SearchKind::Directories;
    auto r = run(dir.path(), c);
    CHECK(relNames(r, dir.path()) == (Names{".hidden", "empty", "sub", "sub/deep"}));
    for (const auto& h : r.hits) CHECK(h.type == EntryType::Directory);
    c.nameContains = "e";
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{".hidden", "empty", "sub/deep"}));
    c.nameContains.clear();
    c.kind = SearchKind::Any;
    c.patterns = {"s*"};
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"sub"}));
    c.patterns.clear();
    c.minSize = 1; // directories have no size: they can never satisfy a size condition
    c.kind = SearchKind::Directories;
    CHECK(run(dir.path(), c).hits.empty());
}

void hiddenEntriesCanBeSkipped() {
    TempDir dir;
    makeSample(dir.path());
    SearchCriteria c;
    c.includeHidden = false;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"B.TXT", "a.txt", "sub/c.cpp", "sub/deep/d.TXT"}));
}

void symlinksAreNeverFollowedNorReported() {
    TempDir dir;
    makeFile(dir.path() / "real" / "x.txt", 1);
    fs::create_directory_symlink(dir.path() / "real", dir.path() / "linkdir");
    fs::create_symlink(dir.path() / "real" / "x.txt", dir.path() / "linkfile");
    fs::create_directory_symlink(dir.path(), dir.path() / "real" / "loop");
    SearchCriteria c;
    c.kind = SearchKind::Any;
    CHECK(relNames(run(dir.path(), c), dir.path()) == (Names{"real", "real/x.txt"}));
}

void resultCapTruncatesAndNeverExceedsTheLimit() {
    TempDir dir;
    for (int d = 0; d < 20; ++d)
        for (int f = 0; f < 50; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    SearchOptions o;
    o.maxResults = 100;
    const auto r = run(dir.path(), {}, o);
    CHECK(r.truncated);
    CHECK_EQ(r.hits.size(), std::size_t{100});
    CHECK(std::is_sorted(r.hits.begin(), r.hits.end(), [](const auto& a, const auto& b) { return a.path < b.path; }));
    o.maxResults = 1000; // exactly all files: not truncated
    const auto all = run(dir.path(), {}, o);
    CHECK(!all.truncated);
    CHECK_EQ(all.hits.size(), std::size_t{1000});
    o.maxResults = 1000;
    SearchCriteria c;
    c.nameContains = "f1";
    CHECK(!run(dir.path(), c, o).truncated);
}

void cancellationAndValidationAndBadRoots() {
    TempDir dir;
    makeSample(dir.path());
    FileSearcher searcher;
    std::stop_source source;
    source.request_stop();
    auto cancelled = searcher.search(dir.path(), {}, {}, {}, {}, source.get_token());
    CHECK(!cancelled && cancelled.error().code == SearchError::Code::Cancelled);

    SearchCriteria bad;
    bad.minSize = 10;
    bad.maxSize = 5;
    auto r = searcher.search(dir.path(), bad);
    CHECK(!r && r.error().code == SearchError::Code::InvalidCriteria);
    bad = {};
    bad.modifiedAfter = 5; bad.modifiedBefore = 1;
    CHECK(FileSearcher::validate(bad).has_value());
    bad = {};
    bad.createdAfter = 5; bad.createdBefore = 1;
    CHECK(FileSearcher::validate(bad).has_value());
    bad = {};
    bad.patterns = {""};
    CHECK(FileSearcher::validate(bad).has_value());
    CHECK(!FileSearcher::validate({}).has_value());

    r = searcher.search(dir.path() / "missing", {});
    CHECK(!r && r.error().code == SearchError::Code::InvalidPath);
    r = searcher.search(dir.path() / "a.txt", {});
    CHECK(!r && r.error().code == SearchError::Code::NotADirectory);
}

void midSearchCancellationStopsPromptly() {
    TempDir dir;
    for (int d = 0; d < 30; ++d)
        for (int f = 0; f < 30; ++f) makeFile(dir.path() / ("d" + std::to_string(d)) / ("f" + std::to_string(f)), 1);
    std::stop_source source;
    std::atomic<int> batches{0};
    auto result = FileSearcher{}.search(dir.path(), {}, {}, {}, [&](std::span<const SearchHit>) {
        if (batches.fetch_add(1) == 2) source.request_stop();
    }, source.get_token());
    CHECK(!result && result.error().code == SearchError::Code::Cancelled);
    CHECK(batches.load() < 31); // not every directory was reported
}

void liveHitsEqualFinalHits() {
    TempDir dir;
    makeSample(dir.path());
    std::vector<std::string> live;
    std::atomic<int> concurrent{0};
    std::atomic<bool> overlapped{false};
    SearchOptions o;
    o.threads = 4;
    std::vector<SearchProgress> progress;
    auto result = FileSearcher{}.search(dir.path(), {}, o,
        [&](const SearchProgress& p) { progress.push_back(p); },
        [&](std::span<const SearchHit> batch) {
            if (concurrent.fetch_add(1) != 0) overlapped = true;
            for (const auto& h : batch) live.push_back(h.path);
            concurrent.fetch_sub(1);
        });
    CHECK(result.has_value());
    CHECK(!overlapped.load());
    std::sort(live.begin(), live.end());
    std::vector<std::string> final;
    for (const auto& h : result->hits) final.push_back(h.path);
    CHECK(live == final);
    CHECK(!progress.empty());
    CHECK(progress.back().currentPath.empty());
    CHECK_EQ(progress.back().matches, std::uint64_t{5});
}

void externalExecutorIsUsed() {
    TempDir dir;
    makeSample(dir.path());
    StdThreadPool pool(2);
    auto result = FileSearcher{}.search(dir.path(), {}, {}, {}, {}, {}, &pool);
    CHECK(result.has_value());
    CHECK_EQ(result->hits.size(), std::size_t{5});
}

void invalidUtf8NamesAreRepaired() {
    TempDir dir;
    const std::string raw = std::string("bad-\xFF-name.txt");
    { std::ofstream out(dir.path().string() + "/" + raw); }
    const auto r = run(dir.path(), {});
    CHECK_EQ(r.hits.size(), std::size_t{1});
    CHECK_EQ(r.hits.at(0).name, std::string("bad-\xEF\xBF\xBD-name.txt"));
    SearchCriteria c;
    c.patterns = {"bad-?-name.txt"};
    CHECK_EQ(run(dir.path(), c).hits.size(), std::size_t{1});
}

void unreadableDirectoriesAreCountedNotFatal() {
    if (::geteuid() == 0) { std::cerr << "  note: running as root, permission test skipped\n"; return; }
    TempDir dir;
    makeSample(dir.path());
    fs::permissions(dir.path() / "sub", fs::perms::none);
    const auto r = run(dir.path(), {});
    CHECK_EQ(r.inaccessible, std::uint64_t{1});
    CHECK(relNames(r, dir.path()) == (Names{".hidden/e.txt", "B.TXT", "a.txt"}));
}

void searchNeverModifiesTheTree() {
    TempDir dir;
    makeSample(dir.path());
    const auto before = [&] {
        std::set<std::string> s;
        for (const auto& e : fs::recursive_directory_iterator(dir.path())) s.insert(e.path().string());
        return s;
    };
    const auto a = before();
    SearchCriteria c;
    c.createdAfter = 0;
    run(dir.path(), c);
    CHECK(a == before());
}

int main() {
    RUN(everythingWithoutCriteria);
    RUN(nameSubstringIsCaseInsensitiveByDefault);
    RUN(patternsAreOrCombinedAndExcludesWin);
    RUN(sizeBoundsAreInclusive);
    RUN(modificationDateRange);
    RUN(creationDateRange);
    RUN(creationIsReportedOnlyWhenAsked);
    RUN(kindsAndDirectories);
    RUN(hiddenEntriesCanBeSkipped);
    RUN(symlinksAreNeverFollowedNorReported);
    RUN(resultCapTruncatesAndNeverExceedsTheLimit);
    RUN(cancellationAndValidationAndBadRoots);
    RUN(midSearchCancellationStopsPromptly);
    RUN(liveHitsEqualFinalHits);
    RUN(externalExecutorIsUsed);
    RUN(invalidUtf8NamesAreRepaired);
    RUN(unreadableDirectoriesAreCountedNotFatal);
    RUN(searchNeverModifiesTheTree);
    return testReport("search_tests");
}
