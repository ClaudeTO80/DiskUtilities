/**
 * @file WebUiAdapter.cpp
 * @brief WebUiAdapter implementation: the du_* JavaScript bindings.
 * @details Each binding parses its arguments defensively, calls a thread-safe collaborator and returns a JSON
 *          string; exceptions never cross the WebUI boundary.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "WebUiAdapter.h"

#include "diskutilities/filesystem/FolderBrowser.h"
#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace diskutilities::web {
namespace {

std::filesystem::path normalizedAbsolute(const std::filesystem::path& path) {
    std::error_code ec;
    auto result = std::filesystem::absolute(path, ec);
    if (ec) throw std::filesystem::filesystem_error("Percorso non valido", path, ec);
    return result.lexically_normal();
}

} // namespace

WebUiAdapter::WebUiAdapter(webui::window& window, SnapshotBridge& bridge, ScanController& scans,
                           SearchController& searches, std::filesystem::path defaultRoot)
    : window_(window), bridge_(bridge), scans_(scans), searches_(searches), defaultRoot_(normalizedAbsolute(defaultRoot)) {}

std::string WebUiAdapter::eventString(webui::window::event* event, std::size_t index, const std::string& fallback) {
    if (event == nullptr || event->get_count() <= index) return fallback;
    return event->get_string(index);
}

long long WebUiAdapter::eventInt(webui::window::event* event, std::size_t index, long long fallback) {
    if (event == nullptr || event->get_count() <= index) return fallback;
    return event->get_int(index);
}

std::filesystem::path WebUiAdapter::rootArgument(webui::window::event* event) const {
    const auto text = eventString(event, 0, pathToUtf8(defaultRoot_));
    return normalizedAbsolute(pathFromUtf8(text.empty() ? pathToUtf8(defaultRoot_) : text));
}

std::string WebUiAdapter::jsonResult(const std::function<Json::Value()>& operation) {
    try {
        return writeJson(operation());
    } catch (const std::exception& ex) {
        return errorJson(ex.what());
    } catch (...) {
        return errorJson("Errore nativo sconosciuto.");
    }
}

void WebUiAdapter::bind() {
    window_.bind("du_getDefaultRoot", this, &WebUiAdapter::getDefaultRoot);
    window_.bind("du_getDashboard", this, &WebUiAdapter::getDashboard);
    window_.bind("du_getLargestFiles", this, &WebUiAdapter::getLargestFiles);
    window_.bind("du_getExtensionStatistics", this, &WebUiAdapter::getExtensionStatistics);
    window_.bind("du_listDirectory", this, &WebUiAdapter::listDirectory);
    window_.bind("du_getDirectoryTree", this, &WebUiAdapter::getDirectoryTree);
    window_.bind("du_startScan", this, &WebUiAdapter::startScan);
    window_.bind("du_cancelScan", this, &WebUiAdapter::cancelScan);
    window_.bind("du_getScanStatus", this, &WebUiAdapter::getScanStatus);
    window_.bind("du_listFolders", this, &WebUiAdapter::listFolders);
    window_.bind("du_startSearch", this, &WebUiAdapter::startSearch);
    window_.bind("du_cancelSearch", this, &WebUiAdapter::cancelSearch);
    window_.bind("du_getSearchStatus", this, &WebUiAdapter::getSearchStatus);
    window_.bind("du_getSearchResults", this, &WebUiAdapter::getSearchResults);
}

void WebUiAdapter::getDefaultRoot(webui::window::event* e) {
    e->return_string(jsonResult([&] { return Json::Value(pathToUtf8(defaultRoot_)); }));
}

void WebUiAdapter::listFolders(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        // Empty text: the top of the file system. Folder browsing never touches the scan state.
        const auto text = eventString(e, 0, {});
        return toJson(diskutilities::listFolders(text.empty() ? std::filesystem::path() : pathFromUtf8(text)),
                      folderShortcuts());
    }));
}

void WebUiAdapter::getDashboard(webui::window::event* e) {
    e->return_string(jsonResult([&] { return toJson(bridge_.getDashboard(rootArgument(e))); }));
}

void WebUiAdapter::getLargestFiles(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        const auto limit = static_cast<std::size_t>(std::clamp<long long>(eventInt(e, 1, 100), 0, 10000));
        return toJson(bridge_.getLargestFiles(rootArgument(e), limit));
    }));
}

void WebUiAdapter::getExtensionStatistics(webui::window::event* e) {
    e->return_string(jsonResult([&] { return toJson(bridge_.getExtensionStatistics(rootArgument(e))); }));
}

void WebUiAdapter::listDirectory(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        auto options = parseListOptions(eventString(e, 1));
        if (!options) throw std::invalid_argument(options.error());
        return toJson(bridge_.listDirectory(rootArgument(e), *options));
    }));
}

void WebUiAdapter::getDirectoryTree(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        const auto depth = static_cast<std::size_t>(std::clamp<long long>(eventInt(e, 1, 2), 0, 64));
        return toJson(bridge_.getDirectoryTree(rootArgument(e), depth));
    }));
}

void WebUiAdapter::startScan(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        Json::Value response(Json::objectValue);
        const auto root = rootArgument(e);
        if (auto started = scans_.start(root); !started) {
            response["error"] = started.error();
        } else {
            response["started"] = true;
            response["root"] = pathToUtf8(root);
        }
        return response;
    }));
}

void WebUiAdapter::cancelScan(webui::window::event* e) {
    scans_.cancel();
    Json::Value response(Json::objectValue);
    response["requested"] = true;
    e->return_string(writeJson(response));
}

void WebUiAdapter::getScanStatus(webui::window::event* e) {
    e->return_string(jsonResult([&] { return toJson(scans_.status()); }));
}

void WebUiAdapter::startSearch(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        Json::Value response(Json::objectValue);
        auto request = parseSearchRequest(eventString(e, 1));
        if (!request) {
            response["error"] = request.error();
            return response;
        }
        const auto root = rootArgument(e);
        if (auto started = searches_.start(root, std::move(request->criteria), request->options); !started) {
            response["error"] = started.error();
        } else {
            response["started"] = true;
            response["root"] = pathToUtf8(root);
        }
        return response;
    }));
}

void WebUiAdapter::cancelSearch(webui::window::event* e) {
    searches_.cancel();
    Json::Value response(Json::objectValue);
    response["requested"] = true;
    e->return_string(writeJson(response));
}

void WebUiAdapter::getSearchStatus(webui::window::event* e) {
    e->return_string(jsonResult([&] { return toJson(searches_.status()); }));
}

void WebUiAdapter::getSearchResults(webui::window::event* e) {
    e->return_string(jsonResult([&] {
        auto request = parseSearchPageRequest(eventString(e, 0));
        if (!request) throw std::invalid_argument(request.error());
        return toJson(searches_.results(request->offset, request->limit, request->sort, request->descending));
    }));
}

} // namespace diskutilities::web
