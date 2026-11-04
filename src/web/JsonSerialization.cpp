/**
 * @file JsonSerialization.cpp
 * @brief JsonCpp serialization of the bridge DTOs.
 * @details Output is emitted as UTF-8 without escaping non-ASCII characters; sizes are written as unsigned
 *          64-bit integers.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "JsonSerialization.h"

#include "diskutilities/web/SnapshotBridge.h"

#include <algorithm>
#include "diskutilities/filesystem/PathUtils.h"

#include <memory>
#include <optional>

namespace diskutilities::web {

Json::Value toJson(const DirectoryEntryDto& v) {
    Json::Value j(Json::objectValue);
    j["name"] = v.name;
    j["path"] = v.path;
    j["type"] = v.type;
    j["extension"] = v.extension;
    j["size"] = Json::UInt64(v.size);
    j["fileCount"] = Json::UInt64(v.fileCount);
    j["modified"] = v.modified;
    j["accessible"] = v.accessible;
    j["hasSubdirectories"] = v.hasSubdirectories;
    return j;
}

Json::Value toJson(const DirectorySizeDto& v) {
    Json::Value j(Json::objectValue);
    j["name"] = v.name;
    j["path"] = v.path;
    j["size"] = Json::UInt64(v.size);
    return j;
}

Json::Value toJson(const VolumeDto& v) {
    Json::Value j(Json::objectValue);
    j["valid"] = v.valid;
    j["capacity"] = Json::UInt64(v.capacity);
    j["free"] = Json::UInt64(v.free);
    j["available"] = Json::UInt64(v.available);
    return j;
}

Json::Value toJson(const DashboardDto& v) {
    Json::Value j(Json::objectValue);
    j["scanned"] = v.scanned;
    j["root"] = v.root;
    j["totalBytes"] = Json::UInt64(v.totalBytes);
    j["fileCount"] = Json::UInt64(v.fileCount);
    j["directoryCount"] = Json::UInt64(v.directoryCount);
    j["symlinkCount"] = Json::UInt64(v.symlinkCount);
    j["inaccessibleCount"] = Json::UInt64(v.inaccessibleCount);
    j["scanDurationMs"] = Json::UInt64(v.scanDurationMs);
    j["volume"] = toJson(v.volume);
    j["largestDirectories"] = Json::Value(Json::arrayValue);
    for (const auto& d : v.largestDirectories) j["largestDirectories"].append(toJson(d));
    return j;
}

Json::Value toJson(const ExtensionStatisticsDto& v) {
    Json::Value j(Json::objectValue);
    j["extension"] = v.extension;
    j["count"] = Json::UInt64(v.count);
    j["totalBytes"] = Json::UInt64(v.totalBytes);
    return j;
}

Json::Value toJson(const DirectoryTreeDto& v) {
    Json::Value j(Json::objectValue);
    j["name"] = v.name;
    j["path"] = v.path;
    j["size"] = Json::UInt64(v.size);
    j["children"] = Json::Value(Json::arrayValue);
    for (const auto& child : v.children) j["children"].append(toJson(child));
    return j;
}

Json::Value toJson(const FileDto& v) {
    Json::Value j(Json::objectValue);
    j["name"] = v.name;
    j["path"] = v.path;
    j["extension"] = v.extension;
    j["size"] = Json::UInt64(v.size);
    j["modified"] = v.modified;
    return j;
}

Json::Value toJson(const DirectoryListingDto& v) {
    Json::Value j(Json::objectValue);
    j["found"] = v.found;
    j["path"] = v.path;
    j["total"] = Json::UInt64(v.total);
    j["totalBytes"] = Json::UInt64(v.totalBytes);
    j["offset"] = Json::UInt64(v.offset);
    j["truncated"] = v.truncated;
    j["entries"] = Json::Value(Json::arrayValue);
    for (const auto& entry : v.entries) j["entries"].append(toJson(entry));
    return j;
}

Json::Value toJson(const ScanStatus& s) {
    Json::Value j(Json::objectValue);
    j["state"] = toString(s.state);
    j["phase"] = s.phase;
    j["root"] = s.root;
    j["currentPath"] = s.currentPath;
    j["error"] = s.error;
    j["entriesVisited"] = Json::UInt64(s.entriesVisited);
    j["filesFound"] = Json::UInt64(s.filesFound);
    j["directoriesFound"] = Json::UInt64(s.directoriesFound);
    j["inaccessibleFound"] = Json::UInt64(s.inaccessibleFound);
    j["elapsedMs"] = Json::UInt64(s.elapsedMs);
    j["bytesFound"] = Json::UInt64(s.bytesFound);
    j["volume"] = toJson(VolumeDto{s.volume.valid, s.volume.capacity, s.volume.free, s.volume.available});
    if (s.state == ScanState::Running) {
        // Same field names as the final queries, so the front end renders partial and final data alike.
        const auto label = [](const std::string& extension) {
            return extension.empty() ? std::string(Snapshot::kNoExtension) : extension;
        };
        Json::Value live(Json::objectValue);
        live["extensionCount"] = Json::UInt64(s.live.extensionCount);
        Json::Value extensions(Json::arrayValue);
        for (const auto& e : s.live.extensions) {
            Json::Value item(Json::objectValue);
            item["extension"] = label(e.extension);
            item["count"] = Json::UInt64(e.count);
            item["totalBytes"] = Json::UInt64(e.bytes);
            extensions.append(std::move(item));
        }
        live["extensions"] = std::move(extensions);
        Json::Value directories(Json::arrayValue);
        for (const auto& d : s.live.directories) {
            Json::Value item(Json::objectValue);
            item["name"] = d.name;
            item["path"] = d.path;
            item["size"] = Json::UInt64(d.bytes);
            directories.append(std::move(item));
        }
        live["directories"] = std::move(directories);
        Json::Value files(Json::arrayValue);
        for (const auto& f : s.live.largestFiles) {
            Json::Value item(Json::objectValue);
            item["name"] = f.name;
            item["path"] = f.path;
            item["extension"] = label(f.extension);
            item["size"] = Json::UInt64(f.size);
            item["modified"] = SnapshotBridge::formatTimestamp(f.modified);
            files.append(std::move(item));
        }
        live["largestFiles"] = std::move(files);
        j["live"] = std::move(live);
    }
    return j;
}

Json::Value toJson(const FolderListing& listing, const std::vector<FolderShortcut>& shortcuts) {
    Json::Value j(Json::objectValue);
    j["found"] = listing.found;
    j["path"] = listing.path;
    j["parent"] = listing.parent;
    j["canGoUp"] = listing.canGoUp;
    j["truncated"] = listing.truncated;
    j["reason"] = listing.error; // not "error": the front end treats that key as a failed call
    Json::Value folders(Json::arrayValue);
    for (const auto& f : listing.folders) {
        Json::Value item(Json::objectValue);
        item["name"] = f.name;
        item["path"] = f.path;
        item["hidden"] = f.hidden;
        folders.append(std::move(item));
    }
    j["folders"] = std::move(folders);
    Json::Value places(Json::arrayValue);
    for (const auto& s : shortcuts) {
        Json::Value item(Json::objectValue);
        item["name"] = s.name;
        item["path"] = s.path;
        places.append(std::move(item));
    }
    j["shortcuts"] = std::move(places);
    return j;
}

Json::Value toJson(const std::vector<ExtensionStatisticsDto>& v) {
    Json::Value j(Json::arrayValue);
    for (const auto& x : v) j.append(toJson(x));
    return j;
}

Json::Value toJson(const std::vector<FileDto>& v) {
    Json::Value j(Json::arrayValue);
    for (const auto& x : v) j.append(toJson(x));
    return j;
}

std::string writeJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    builder["emitUTF8"] = true; // keep non-ASCII names readable and the payload smaller
    return Json::writeString(builder, value);
}

std::string errorJson(const std::string& message) {
    Json::Value value(Json::objectValue);
    value["error"] = message;
    return writeJson(value);
}

Json::Value toJson(const SearchStatus& s) {
    Json::Value j(Json::objectValue);
    j["state"] = toString(s.state);
    j["root"] = s.root;
    j["currentPath"] = s.currentPath;
    j["failure"] = s.error; // not "error": the front end turns that key into a thrown failure
    j["examined"] = Json::UInt64(s.examined);
    j["directories"] = Json::UInt64(s.directories);
    j["inaccessible"] = Json::UInt64(s.inaccessible);
    j["matches"] = Json::UInt64(s.matches);
    j["elapsedMs"] = Json::UInt64(s.elapsedMs);
    j["truncated"] = s.truncated;
    j["creationUnavailable"] = Json::UInt64(s.creationUnavailable);
    return j;
}

Json::Value toJson(const SearchPage& page) {
    Json::Value j(Json::objectValue);
    j["total"] = Json::UInt64(page.total);
    Json::Value hits(Json::arrayValue);
    for (const auto& h : page.hits) {
        Json::Value item(Json::objectValue);
        item["name"] = h.name;
        item["path"] = h.path;
        item["type"] = h.type == EntryType::Directory ? "directory" : "file";
        item["extension"] = h.type == EntryType::Directory ? std::string() : std::string(rawExtension(h.name));
        item["size"] = Json::UInt64(h.size);
        item["modified"] = SnapshotBridge::formatTimestamp(h.modified);
        item["created"] = SnapshotBridge::formatTimestamp(h.created);
        hits.append(std::move(item));
    }
    j["hits"] = std::move(hits);
    return j;
}

namespace {

Json::Value parseObject(const std::string& text, std::string& problem) {
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors) || !root.isObject())
        problem = "atteso un oggetto JSON";
    return root;
}

/** Optional integer member: absent or null -> nullopt; non-integral or out-of-range -> sets @p problem. */
std::optional<std::int64_t> optionalInt(const Json::Value& root, const char* key, bool nonNegative, std::string& problem) {
    const Json::Value& v = root[key];
    if (v.isNull()) return std::nullopt;
    if (!v.isNumeric()) { problem = std::string("Campo non numerico: ") + key; return std::nullopt; }
    const double d = v.asDouble();
    if (!(d > -9.0e15 && d < 9.0e15)) { problem = std::string("Valore fuori intervallo: ") + key; return std::nullopt; }
    if (nonNegative && d < 0) { problem = std::string("Valore negativo non ammesso: ") + key; return std::nullopt; }
    return static_cast<std::int64_t>(d);
}

std::vector<std::string> stringList(const Json::Value& v, const char* key, std::string& problem) {
    std::vector<std::string> out;
    if (v.isNull()) return out;
    if (!v.isArray()) { problem = std::string("Atteso un elenco: ") + key; return out; }
    for (const auto& item : v) {
        if (!item.isString()) { problem = std::string("Elemento non testuale in ") + key; return {}; }
        out.push_back(item.asString());
    }
    return out;
}

} // namespace

std::expected<SearchRequest, std::string> parseSearchRequest(const std::string& text) {
    std::string problem;
    const Json::Value root = parseObject(text, problem);
    if (!problem.empty()) return std::unexpected("Richiesta di ricerca non valida: " + problem + ".");

    SearchRequest request;
    request.options.reportCreation = true; // the results table has a creation column
    SearchCriteria& c = request.criteria;
    const auto fail = [&]() { return std::unexpected("Richiesta di ricerca non valida: " + problem); };
    try {
        c.nameContains = root.get("nameContains", "").asString();
        c.patterns = stringList(root["patterns"], "patterns", problem);
        c.excludePatterns = stringList(root["excludePatterns"], "excludePatterns", problem);
        c.caseSensitive = root.get("caseSensitive", false).asBool();
        c.includeHidden = root.get("includeHidden", true).asBool();
        request.options.stayOnFileSystem = root.get("stayOnFileSystem", false).asBool();
        const std::string kind = root.get("kind", "files").asString();
        if (kind == "files") c.kind = SearchKind::Files;
        else if (kind == "directories") c.kind = SearchKind::Directories;
        else if (kind == "any") c.kind = SearchKind::Any;
        else return std::unexpected("Richiesta di ricerca non valida: tipo di elemento sconosciuto: " + kind);
        if (!problem.empty()) return fail();

        if (auto v = optionalInt(root, "minSize", true, problem)) c.minSize = static_cast<std::uint64_t>(*v);
        if (auto v = optionalInt(root, "maxSize", true, problem)) c.maxSize = static_cast<std::uint64_t>(*v);
        c.modifiedAfter = optionalInt(root, "modifiedAfter", false, problem);
        c.modifiedBefore = optionalInt(root, "modifiedBefore", false, problem);
        c.createdAfter = optionalInt(root, "createdAfter", false, problem);
        c.createdBefore = optionalInt(root, "createdBefore", false, problem);
        if (auto v = optionalInt(root, "maxResults", false, problem))
            request.options.maxResults = static_cast<std::uint64_t>(std::clamp<std::int64_t>(*v, 1, 100000));
        if (!problem.empty()) return fail();
    } catch (const std::exception&) {
        return std::unexpected("Richiesta di ricerca non valida: tipo di campo errato.");
    }
    return request;
}

std::expected<SearchPageRequest, std::string> parseSearchPageRequest(const std::string& text) {
    SearchPageRequest request;
    if (text.empty()) return request;
    std::string problem;
    const Json::Value root = parseObject(text, problem);
    if (!problem.empty()) return std::unexpected("Opzioni dei risultati non valide: " + problem + ".");
    try {
        if (auto v = optionalInt(root, "offset", true, problem)) request.offset = static_cast<std::size_t>(*v);
        if (auto v = optionalInt(root, "limit", false, problem))
            request.limit = static_cast<std::size_t>(std::clamp<std::int64_t>(*v, 1, 5000));
        if (!problem.empty()) return std::unexpected("Opzioni dei risultati non valide: " + problem);
        request.sort = parseSearchSort(root.get("sort", "path").asString());
        request.descending = root.get("descending", false).asBool();
    } catch (const std::exception&) {
        return std::unexpected("Opzioni dei risultati non valide: tipo di campo errato.");
    }
    return request;
}

std::expected<ListOptions, std::string> parseListOptions(const std::string& text) {
    ListOptions options;
    if (text.empty()) return options;

    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors) || !root.isObject())
        return std::unexpected("Opzioni di elenco non valide: atteso un oggetto JSON.");

    try {
    const std::string sort = root.get("sort", "name").asString();
    if (sort == "name") options.sort = SortKey::Name;
    else if (sort == "size") options.sort = SortKey::Size;
    else if (sort == "modified") options.sort = SortKey::Modified;
    else if (sort == "type") options.sort = SortKey::Type;
    else return std::unexpected("Criterio di ordinamento sconosciuto: " + sort);

    options.descending = root.get("descending", false).asBool();
    options.filter = root.get("filter", "").asString();
    options.directoriesOnly = root.get("directoriesOnly", false).asBool();

    const auto clampCount = [](const Json::Value& value, std::size_t fallback, std::size_t lo, std::size_t hi) {
        if (!value.isNumeric()) return fallback;
        const double number = value.asDouble();
        if (!(number >= static_cast<double>(lo))) return lo; // also catches NaN
        if (number >= static_cast<double>(hi)) return hi;
        return static_cast<std::size_t>(number);
    };
    options.offset = clampCount(root["offset"], 0, 0, 100'000'000);
    options.limit = clampCount(root["limit"], 2000, 1, 10000);
    return options;
    } catch (const std::exception&) {
        // Json::Value::asString()/asBool() throw on a wrongly typed member.
        return std::unexpected("Opzioni di elenco non valide: tipo di campo errato.");
    }
}

} // namespace diskutilities::web
