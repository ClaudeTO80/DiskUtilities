#pragma once

/**
 * @file JsonSerialization.h
 * @brief JsonCpp (de)serialization of the bridge DTOs.
 * @details Private to the applications: the core library does not depend on JsonCpp. Field names are the
 *          contract with web/app.js.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FolderBrowser.h"
#include "diskutilities/web/BridgeContract.h"
#include "diskutilities/web/ScanController.h"
#include "diskutilities/web/SearchController.h"

#include <json/json.h>

#include <expected>
#include <string>
#include <vector>

namespace diskutilities::web {

/** @brief Serialize a directory entry to a JSON object. @param value Source DTO. */
Json::Value toJson(const DirectoryEntryDto& value);
/** @brief Serialize a directory size to a JSON object. @param value Source DTO. */
Json::Value toJson(const DirectorySizeDto& value);
/** @brief Serialize the volume capacity to a JSON object. @param value Source DTO. */
Json::Value toJson(const VolumeDto& value);
/** @brief Serialize the dashboard to a JSON object. @param value Source DTO. */
Json::Value toJson(const DashboardDto& value);
/** @brief Serialize an extension statistic to a JSON object. @param value Source DTO. */
Json::Value toJson(const ExtensionStatisticsDto& value);
/** @brief Serialize a directory tree node (recursive) to a JSON object. @param value Source DTO. */
Json::Value toJson(const DirectoryTreeDto& value);
/** @brief Serialize a file entry to a JSON object. @param value Source DTO. */
Json::Value toJson(const FileDto& value);
/** @brief Serialize a directory listing page to a JSON object. @param value Source DTO. */
Json::Value toJson(const DirectoryListingDto& value);
/** @brief Serialize the scan status to a JSON object. @param value Source DTO. */
Json::Value toJson(const ScanStatus& value);
/** @brief Serialize extension statistics to a JSON array. @param value Source DTOs. */
Json::Value toJson(const std::vector<ExtensionStatisticsDto>& value);
/** @brief Serialize a file ranking to a JSON array. @param value Source DTOs. */
Json::Value toJson(const std::vector<FileDto>& value);

/**
 * @brief Serialize a folder listing together with the starting-point shortcuts.
 * @param listing   Result of listFolders().
 * @param shortcuts Result of folderShortcuts().
 * @return JSON object: found, path, parent, canGoUp, truncated, reason (the error text, not named "error" because the front end treats that key as a failed call), folders[], shortcuts[].
 */
Json::Value toJson(const FolderListing& listing, const std::vector<FolderShortcut>& shortcuts);

/** @brief Serialize the status of a search to a JSON object. @param value Source status. */
Json::Value toJson(const SearchStatus& value);
/** @brief Serialize one page of matches (total + hits[] with ISO-8601 timestamps, "" when unknown). @param value Source page. */
Json::Value toJson(const SearchPage& value);

/** @brief A parsed `du_startSearch` request. */
struct SearchRequest {
    SearchCriteria criteria;  ///< What to look for.
    SearchOptions options;    ///< Result cap and traversal options.
};

/**
 * @brief Parse the search request sent by the frontend as a JSON object.
 * @details Keys: nameContains, patterns[], excludePatterns[], caseSensitive, kind ("files"|"directories"|"any"),
 *          includeHidden, minSize, maxSize (bytes), modifiedAfter, modifiedBefore, createdAfter, createdBefore
 *          (Unix seconds; null or missing = unbounded), maxResults (clamped to [1, 100000]), stayOnFileSystem.
 *          Unknown keys are ignored; wrongly typed values and negative sizes are rejected with a message.
 * @param json Text received from JavaScript.
 * @return The request, or a message (Italian) describing what is wrong.
 */
std::expected<SearchRequest, std::string> parseSearchRequest(const std::string& json);

/** @brief A parsed `du_getSearchResults` request. */
struct SearchPageRequest {
    std::size_t offset = 0;                 ///< First hit to return.
    std::size_t limit = 500;                ///< Maximum number of hits (clamped to [1, 5000]).
    SearchSort sort = SearchSort::Path;     ///< Ordering column.
    bool descending = false;                ///< Reverse order.
};

/**
 * @brief Parse the paging options of `du_getSearchResults` ({offset, limit, sort, descending}).
 * @param json Text received from JavaScript; empty yields the defaults.
 * @return The options, or a message if the text is not a JSON object.
 */
std::expected<SearchPageRequest, std::string> parseSearchPageRequest(const std::string& json);

/**
 * @brief Compact JSON text of @p value.
 * @param value Document to write.
 */
std::string writeJson(const Json::Value& value);
/**
 * @brief Convenience: `{"error": message}`.
 * @param message Human readable description.
 */
std::string errorJson(const std::string& message);

/**
 * @brief Parse the listing options sent by the frontend as a JSON object.
 * @details Unknown keys are ignored; out-of-range numbers are clamped (limit to
 * [1, 10000]); an empty string yields the defaults.
 * @param json Text received from JavaScript.
 * @return The options, or a message if the text is not a JSON object.
 */
std::expected<ListOptions, std::string> parseListOptions(const std::string& json);

} // namespace diskutilities::web
