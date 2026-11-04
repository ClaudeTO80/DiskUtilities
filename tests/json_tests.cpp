/**
 * @file json_tests.cpp
 * @brief Tests for the JSON serialization and the listing-option parser.
 * @details Covers clamping, type errors and UTF-8 output.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "JsonSerialization.h"

using namespace diskutilities::web;

static Json::Value parse(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    std::istringstream in(text);
    CHECK(Json::parseFromStream(b, in, &v, &errs));
    return v;
}

void listingShape() {
    DirectoryListingDto dto;
    dto.found = true;
    dto.path = "/data";
    dto.total = 3;
    dto.truncated = true;
    DirectoryEntryDto e;
    e.name = "caf\xC3\xA9 \"q\" \\ \n";
    e.path = "/data/x";
    e.type = "directory";
    e.size = 18'000'000'000'000ULL;
    e.hasSubdirectories = true;
    dto.entries.push_back(e);
    const auto j = parse(writeJson(toJson(dto)));
    CHECK(j["found"].asBool());
    CHECK(j["truncated"].asBool());
    CHECK_EQ(j["total"].asUInt64(), std::uint64_t{3});
    CHECK_EQ(j["entries"][0]["name"].asString(), e.name); // escaping round-trips
    CHECK_EQ(j["entries"][0]["size"].asUInt64(), std::uint64_t{18'000'000'000'000ULL});
    CHECK(j["entries"][0]["hasSubdirectories"].asBool());
}

void dashboardShape() {
    DashboardDto d;
    d.scanned = true;
    d.volume = {true, 100, 40, 30};
    d.largestDirectories.push_back({"n", "/p", 5});
    const auto j = parse(writeJson(toJson(d)));
    CHECK(j["scanned"].asBool());
    CHECK_EQ(j["volume"]["available"].asUInt64(), std::uint64_t{30});
    CHECK_EQ(j["largestDirectories"].size(), 1u);
}

void statusShape() {
    ScanStatus s;
    s.state = ScanState::Running;
    s.phase = "scanning";
    s.entriesVisited = 12;
    const auto j = parse(writeJson(toJson(s)));
    CHECK_EQ(j["state"].asString(), std::string("running"));
    CHECK_EQ(j["phase"].asString(), std::string("scanning"));
    CHECK_EQ(j["entriesVisited"].asUInt64(), std::uint64_t{12});
}

void liveStatusShape() {
    ScanStatus s;
    s.state = ScanState::Running;
    s.bytesFound = 123;
    s.volume.valid = true;
    s.volume.capacity = 1000;
    s.live.extensionCount = 2;
    s.live.extensions = {{".txt", 3, 100}, {"", 1, 23}};
    s.live.directories = {{"d", "/r/d", 123}};
    s.live.largestFiles = {{"f.txt", "/r/d/f.txt", ".txt", 100, 0}};
    const auto j = parse(writeJson(toJson(s)));
    CHECK_EQ(j["bytesFound"].asUInt64(), std::uint64_t{123});
    CHECK_EQ(j["volume"]["capacity"].asUInt64(), std::uint64_t{1000});
    CHECK_EQ(j["live"]["extensionCount"].asUInt64(), std::uint64_t{2});
    CHECK_EQ(j["live"]["extensions"][0]["extension"].asString(), std::string(".txt"));
    CHECK_EQ(j["live"]["extensions"][0]["totalBytes"].asUInt64(), std::uint64_t{100});
    CHECK_EQ(j["live"]["extensions"][1]["extension"].asString(), std::string("senza estensione"));
    CHECK_EQ(j["live"]["directories"][0]["size"].asUInt64(), std::uint64_t{123});
    CHECK_EQ(j["live"]["largestFiles"][0]["path"].asString(), std::string("/r/d/f.txt"));
    CHECK_EQ(j["live"]["largestFiles"][0]["modified"].asString(), std::string("1970-01-01T00:00:00Z"));

    s.state = ScanState::Completed; // partial results only exist while running
    CHECK(!parse(writeJson(toJson(s))).isMember("live"));
}

void folderListingShape() {
    diskutilities::FolderListing listing;
    listing.found = true;
    listing.path = "/data";
    listing.parent = "/";
    listing.canGoUp = true;
    listing.folders = {{"caf\xC3\xA9", "/data/caf\xC3\xA9", false}, {".git", "/data/.git", true}};
    const auto j = parse(writeJson(toJson(listing, {{"Cartella utente", "/home/u"}})));
    CHECK(j["found"].asBool());
    CHECK(!j.isMember("error")); // would be read as a failed call by the front end
    CHECK(j.isMember("reason"));
    CHECK_EQ(j["parent"].asString(), std::string("/"));
    CHECK(j["canGoUp"].asBool());
    CHECK_EQ(j["folders"].size(), Json::ArrayIndex{2});
    CHECK_EQ(j["folders"][0]["name"].asString(), std::string("caf\xC3\xA9"));
    CHECK(j["folders"][1]["hidden"].asBool());
    CHECK_EQ(j["shortcuts"][0]["path"].asString(), std::string("/home/u"));
    CHECK(writeJson(toJson(listing, {})).find("caf\xC3\xA9") != std::string::npos); // emitted as UTF-8, not \\u00e9
}

void errorShape() {
    CHECK_EQ(parse(errorJson("boom \"x\""))["error"].asString(), std::string("boom \"x\""));
}

void optionsDefaultsAndClamping() {
    auto o = parseListOptions("");
    CHECK(o.has_value() && o->limit == 2000 && o->offset == 0 && o->sort == diskutilities::SortKey::Name);
    o = parseListOptions(R"({"sort":"size","descending":true,"filter":"abc","directoriesOnly":true,"offset":5,"limit":50})");
    CHECK(o.has_value());
    if (o) {
        CHECK(o->sort == diskutilities::SortKey::Size);
        CHECK(o->descending && o->directoriesOnly);
        CHECK_EQ(o->filter, std::string("abc"));
        CHECK_EQ(o->offset, std::size_t{5});
        CHECK_EQ(o->limit, std::size_t{50});
    }
    o = parseListOptions(R"({"limit":99999999,"offset":-5})");
    CHECK(o.has_value() && o->limit == 10000 && o->offset == 0);
    o = parseListOptions(R"({"limit":0})");
    CHECK(o.has_value() && o->limit == 1);
    o = parseListOptions(R"({"limit":"many"})");
    CHECK(o.has_value() && o->limit == 2000); // non-numeric falls back to the default
}

void optionsRejectBadInput() {
    CHECK(!parseListOptions("[1,2]").has_value());
    CHECK(!parseListOptions("not json").has_value());
    CHECK(!parseListOptions(R"({"sort":"colour"})").has_value());
    CHECK(!parseListOptions(R"({"filter":{"a":1}})").has_value()); // wrong type must not throw
    CHECK(!parseListOptions(R"({"descending":[1]})").has_value());
}

void searchRequestParsing() {
    auto r = parseSearchRequest(R"({"nameContains":"rep","patterns":["*.pdf","a?"],"excludePatterns":["~*"],
        "caseSensitive":true,"kind":"any","includeHidden":false,"minSize":10,"maxSize":2000000000000,
        "modifiedAfter":1000,"modifiedBefore":2000,"createdAfter":-5,"createdBefore":null,"maxResults":500,
        "stayOnFileSystem":true})");
    CHECK(r.has_value());
    if (!r) return;
    const auto& c = r->criteria;
    CHECK_EQ(c.nameContains, std::string("rep"));
    CHECK(c.patterns == (std::vector<std::string>{"*.pdf", "a?"}));
    CHECK(c.excludePatterns == (std::vector<std::string>{"~*"}));
    CHECK(c.caseSensitive);
    CHECK(c.kind == diskutilities::SearchKind::Any);
    CHECK(!c.includeHidden);
    CHECK_EQ(*c.minSize, std::uint64_t{10});
    CHECK_EQ(*c.maxSize, std::uint64_t{2000000000000ULL}); // beyond 32 bits
    CHECK_EQ(*c.modifiedAfter, std::int64_t{1000});
    CHECK_EQ(*c.createdAfter, std::int64_t{-5});          // pre-1970 dates are legal
    CHECK(!c.createdBefore.has_value());
    CHECK_EQ(r->options.maxResults, std::uint64_t{500});
    CHECK(r->options.stayOnFileSystem);
}

void searchRequestDefaultsAndClamping() {
    auto r = parseSearchRequest("{}");
    CHECK(r.has_value());
    CHECK(r->criteria.kind == diskutilities::SearchKind::Files);
    CHECK(r->criteria.includeHidden);
    CHECK(!r->criteria.caseSensitive);
    CHECK(!r->criteria.minSize && !r->criteria.modifiedAfter);
    CHECK_EQ(r->options.maxResults, std::uint64_t{10000});
    CHECK(r->options.reportCreation);
    r = parseSearchRequest(R"({"maxResults":0})");
    CHECK(r.has_value() && r->options.maxResults == 1);
    r = parseSearchRequest(R"({"maxResults":99999999})");
    CHECK(r.has_value() && r->options.maxResults == 100000);
}

void searchRequestRejectsBadInput() {
    CHECK(!parseSearchRequest("").has_value());
    CHECK(!parseSearchRequest("[]").has_value());
    CHECK(!parseSearchRequest(R"({"kind":"links"})").has_value());
    CHECK(!parseSearchRequest(R"({"minSize":-1})").has_value());
    CHECK(!parseSearchRequest(R"({"minSize":"big"})").has_value());
    CHECK(!parseSearchRequest(R"({"patterns":"*.txt"})").has_value());
    CHECK(!parseSearchRequest(R"({"patterns":[1]})").has_value());
    CHECK(!parseSearchRequest(R"({"nameContains":{"a":1}})").has_value());
    CHECK(!parseSearchRequest(R"({"modifiedAfter":1e300})").has_value());
}

void searchPageRequestAndShapes() {
    auto p = parseSearchPageRequest("");
    CHECK(p.has_value() && p->limit == 500 && p->sort == SearchSort::Path && !p->descending);
    p = parseSearchPageRequest(R"({"offset":20,"limit":99999,"sort":"size","descending":true})");
    CHECK(p.has_value());
    CHECK_EQ(p->offset, std::size_t{20});
    CHECK_EQ(p->limit, std::size_t{5000});
    CHECK(p->sort == SearchSort::Size && p->descending);
    CHECK(!parseSearchPageRequest("[1]").has_value());
    CHECK(!parseSearchPageRequest(R"({"offset":-3})").has_value());

    SearchPage page;
    page.total = 7;
    diskutilities::SearchHit hit;
    hit.path = "/d/caf\xC3\xA9.txt";
    hit.name = "caf\xC3\xA9.txt";
    hit.size = 5'000'000'000ULL;
    hit.modified = 86400;
    page.hits.push_back(hit);
    const auto j = parse(writeJson(toJson(page)));
    CHECK_EQ(j["total"].asUInt64(), std::uint64_t{7});
    CHECK_EQ(j["hits"][0]["size"].asUInt64(), std::uint64_t{5'000'000'000ULL});
    CHECK_EQ(j["hits"][0]["name"].asString(), std::string("caf\xC3\xA9.txt"));
    CHECK_EQ(j["hits"][0]["extension"].asString(), std::string(".txt"));
    CHECK_EQ(j["hits"][0]["type"].asString(), std::string("file"));
    CHECK_EQ(j["hits"][0]["created"].asString(), std::string()); // unknown stays empty
    CHECK(!j["hits"][0]["modified"].asString().empty());
    CHECK(!j.isMember("error"));

    SearchStatus st;
    st.state = ScanState::Running;
    st.matches = 3;
    const auto js = parse(writeJson(toJson(st)));
    CHECK_EQ(js["state"].asString(), std::string("running"));
    CHECK_EQ(js["matches"].asUInt64(), std::uint64_t{3});
    CHECK_EQ(js["failure"].asString(), std::string());
    CHECK(!js.isMember("error")); // would make the front end throw for every failed search poll
}

int main() {
    RUN(listingShape);
    RUN(dashboardShape);
    RUN(statusShape);
    RUN(liveStatusShape);
    RUN(folderListingShape);
    RUN(errorShape);
    RUN(optionsDefaultsAndClamping);
    RUN(optionsRejectBadInput);
    RUN(searchRequestParsing);
    RUN(searchRequestDefaultsAndClamping);
    RUN(searchRequestRejectsBadInput);
    RUN(searchPageRequestAndShapes);
    return testReport("json_tests");
}
