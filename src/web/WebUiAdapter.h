#pragma once

/**
 * @file WebUiAdapter.h
 * @brief Binds the bridge, the scan controller and JsonCpp serialization to the WebUI window.
 * @details The adapter is the only place that knows WebUI. Everything below it is plain C++23.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "JsonSerialization.h"
#include "diskutilities/web/ScanController.h"
#include "diskutilities/web/SearchController.h"
#include "diskutilities/web/SnapshotBridge.h"

#include "webui.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace diskutilities::web {

/**
 * @brief Exposes the read-only analysis API to JavaScript (`du_*` bindings).
 * @details WebUI delivers every event on its own thread, so all handlers only touch
 * thread-safe collaborators (SnapshotBridge, ScanController). Each handler
 * returns a JSON string; failures are reported as {"error": "..."}.
 */
class WebUiAdapter final {
public:
    /**
     * @brief Create the adapter; call bind() afterwards.
     * @param window      WebUI window that receives the bindings.
     * @param bridge      Query source.
     * @param scans       Scan lifecycle owner.
     * @param searches    File search lifecycle owner.
     * @param defaultRoot Directory proposed by the UI at start-up.
     */
    WebUiAdapter(webui::window& window, SnapshotBridge& bridge, ScanController& scans,
                 SearchController& searches, std::filesystem::path defaultRoot);
    /** @brief Register every `du_*` function on the window. */
    void bind();

private:
    static std::string eventString(webui::window::event* event, std::size_t index, const std::string& fallback = {});
    static long long eventInt(webui::window::event* event, std::size_t index, long long fallback);
    std::filesystem::path rootArgument(webui::window::event* event) const;
    static std::string jsonResult(const std::function<Json::Value()>& operation);

    void getDefaultRoot(webui::window::event* event);
    void getDashboard(webui::window::event* event);
    void getLargestFiles(webui::window::event* event);
    void getExtensionStatistics(webui::window::event* event);
    void listDirectory(webui::window::event* event);
    void getDirectoryTree(webui::window::event* event);
    void startScan(webui::window::event* event);
    void cancelScan(webui::window::event* event);
    void getScanStatus(webui::window::event* event);
    void listFolders(webui::window::event* event);
    void startSearch(webui::window::event* event);
    void cancelSearch(webui::window::event* event);
    void getSearchStatus(webui::window::event* event);
    void getSearchResults(webui::window::event* event);

    webui::window& window_;
    SnapshotBridge& bridge_;
    ScanController& scans_;
    SearchController& searches_;
    std::filesystem::path defaultRoot_;
};

} // namespace diskutilities::web
