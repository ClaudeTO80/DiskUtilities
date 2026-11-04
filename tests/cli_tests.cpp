/**
 * @file cli_tests.cpp
 * @brief Tests for the `find` command-line parsing (sizes, dates, options).
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "../src/cli/FindArguments.h"

#include <ctime>

using namespace diskutilities;
using namespace diskutilities::cli;

void sizes() {
    CHECK_EQ(*parseSize("0"), std::uint64_t{0});
    CHECK_EQ(*parseSize("500"), std::uint64_t{500});
    CHECK_EQ(*parseSize("500B"), std::uint64_t{500});
    CHECK_EQ(*parseSize("10k"), std::uint64_t{10'000});
    CHECK_EQ(*parseSize("10KB"), std::uint64_t{10'000});
    CHECK_EQ(*parseSize("1.5MB"), std::uint64_t{1'500'000});
    CHECK_EQ(*parseSize("2g"), std::uint64_t{2'000'000'000ULL});
    CHECK_EQ(*parseSize("3T"), std::uint64_t{3'000'000'000'000ULL});
    CHECK_EQ(*parseSize("1KiB"), std::uint64_t{1024});
    CHECK_EQ(*parseSize("2mib"), std::uint64_t{2 * 1024 * 1024});
    CHECK_EQ(*parseSize("1GiB"), std::uint64_t{1ULL << 30});
    CHECK_EQ(*parseSize(".5k"), std::uint64_t{500});
    CHECK(!parseSize("").has_value());
    CHECK(!parseSize("k").has_value());
    CHECK(!parseSize("-5").has_value());
    CHECK(!parseSize("1.2.3").has_value());
    CHECK(!parseSize("5 MB").has_value());
    CHECK(!parseSize("5XB").has_value());
    CHECK(!parseSize("99999999999999999999").has_value()); // does not fit
}

static std::int64_t local(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&tm));
}

void dates() {
    CHECK_EQ(*parseWhen("2026-03-15", false), local(2026, 3, 15));
    CHECK_EQ(*parseWhen("2026-03-15", true), local(2026, 3, 15, 23, 59, 59));
    CHECK_EQ(*parseWhen("2026-03-15T10:30", true), local(2026, 3, 15, 10, 30)); // explicit time wins over endOfDay
    CHECK_EQ(*parseWhen("2026-03-15 10:30:45", false), local(2026, 3, 15, 10, 30, 45));
    CHECK(!parseWhen("2026-02-31", false).has_value());   // not normalised into March
    CHECK(!parseWhen("2026-13-01", false).has_value());
    CHECK(!parseWhen("2026-03-15T25:00", false).has_value());
    CHECK(!parseWhen("2026-03-15T10", false).has_value());
    CHECK(!parseWhen("15/03/2026", false).has_value());
    CHECK(!parseWhen("2026-03-15x", false).has_value());
    CHECK(!parseWhen("", false).has_value());
    CHECK(!parseWhen("d", false).has_value());
    CHECK(!parseWhen("-3d", false).has_value());

    const auto now = std::chrono::system_clock::from_time_t(1'000'000'000);
    CHECK_EQ(*parseWhen("30s", false, now), std::int64_t{1'000'000'000 - 30});
    CHECK_EQ(*parseWhen("5m", false, now), std::int64_t{1'000'000'000 - 300});
    CHECK_EQ(*parseWhen("2h", false, now), std::int64_t{1'000'000'000 - 7200});
    CHECK_EQ(*parseWhen("7d", false, now), std::int64_t{1'000'000'000 - 7 * 86400});
    CHECK_EQ(*parseWhen("2w", false, now), std::int64_t{1'000'000'000 - 14 * 86400});
}

void findParsing() {
    auto r = parseFindArguments({"--name", "rep", "--pattern", "*.pdf", "--pattern", "*.doc", "--exclude", "~*",
                                 "--type", "any", "--min-size", "1MB", "--max-size", "2GiB", "--modified-after", "2026-01-01",
                                 "--modified-before", "2026-01-31", "--created-after", "7d", "--max-results", "50",
                                 "--threads", "2", "--one-file-system", "--no-hidden", "--case-sensitive", "-l", "/data"});
    CHECK(r.has_value());
    if (!r) { std::cerr << r.error() << '\n'; return; }
    CHECK_EQ(r->root.string(), std::string("/data"));
    CHECK_EQ(r->criteria.nameContains, std::string("rep"));
    CHECK(r->criteria.patterns == (std::vector<std::string>{"*.pdf", "*.doc"}));
    CHECK(r->criteria.excludePatterns == (std::vector<std::string>{"~*"}));
    CHECK(r->criteria.kind == SearchKind::Any);
    CHECK_EQ(*r->criteria.minSize, std::uint64_t{1'000'000});
    CHECK_EQ(*r->criteria.maxSize, std::uint64_t{2ULL << 30});
    CHECK(*r->criteria.modifiedBefore > *r->criteria.modifiedAfter);
    CHECK(r->criteria.createdAfter.has_value());
    CHECK_EQ(r->options.maxResults, std::uint64_t{50});
    CHECK_EQ(r->options.threads, 2u);
    CHECK(r->options.stayOnFileSystem && !r->criteria.includeHidden && r->criteria.caseSensitive);
    CHECK(r->format == FindFormat::Long);

    r = parseFindArguments({"-h"});
    CHECK(r.has_value() && r->help);
    r = parseFindArguments({"-0", "/x"});
    CHECK(r.has_value() && r->nulSeparated && r->format == FindFormat::Paths);
}

void findRejectsBadInput() {
    CHECK(!parseFindArguments({}).has_value());                                   // no directory
    CHECK(!parseFindArguments({"/a", "/b"}).has_value());                         // two directories
    CHECK(!parseFindArguments({"--bogus", "/a"}).has_value());
    CHECK(!parseFindArguments({"--name"}).has_value());                           // missing value
    CHECK(!parseFindArguments({"--type", "x", "/a"}).has_value());
    CHECK(!parseFindArguments({"--min-size", "big", "/a"}).has_value());
    CHECK(!parseFindArguments({"--min-size", "2k", "--max-size", "1k", "/a"}).has_value());   // min > max
    CHECK(!parseFindArguments({"--modified-after", "2026-02-01", "--modified-before", "2026-01-01", "/a"}).has_value());
    CHECK(!parseFindArguments({"--max-results", "0", "/a"}).has_value());
    CHECK(!parseFindArguments({"--max-results", "100001", "/a"}).has_value());
    CHECK(!parseFindArguments({"--threads", "65", "/a"}).has_value());
    CHECK(!parseFindArguments({"--pattern", "", "/a"}).has_value());              // empty pattern
    CHECK(!parseFindArguments({"--type", "d", "--min-size", "1", "/a"}).has_value()); // directories have no size
    CHECK(!parseFindArguments({"--json", "-0", "/a"}).has_value());
}

int main() {
    RUN(sizes);
    RUN(dates);
    RUN(findParsing);
    RUN(findRejectsBadInput);
    return testReport("cli_tests");
}
