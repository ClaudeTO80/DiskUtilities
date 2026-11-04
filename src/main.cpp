/**
 * @file main.cpp
 * @brief DiskUtilities desktop application: WebUI window + native analysis backend.
 * @details Locates the web front end, wires SnapshotBridge, ScanController and WebUiAdapter together and opens
 *          the WebUI window. When built with TK4CPP the scan runs on a Tk4cppExecutor and the node limit is
 *          derived from the free memory.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "WebUiAdapter.h"

#include "diskutilities/web/ScanController.h"
#include "diskutilities/web/SearchController.h"
#include "diskutilities/web/SnapshotBridge.h"

#include "webui.hpp"

#if defined(DISKUTILITIES_HAS_TK4CPP)
#include "diskutilities/tk4cpp/Tk4cppSupport.h"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

/** Start in the user's home rather than the (possibly System32 or build) working directory. */
std::filesystem::path defaultUserDirectory() {
#if defined(_WIN32)
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home) return std::filesystem::path(home);
#else
    if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home);
#endif
    return std::filesystem::current_path();
}

/** First candidate directory that contains index.html. */
std::filesystem::path findFrontend() {
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::current_path() / "web",
        std::filesystem::path(DISKUTILITIES_WEB_DIR),
        std::filesystem::path(DISKUTILITIES_INSTALL_WEB_DIR),
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate / "index.html", ec)) return candidate;
    }
    return {};
}

} // namespace

/**
 * @brief Program entry point of the GUI.
 * @details Optional first argument: the directory proposed at start-up (default: the user's home).
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on success, 1 on an unexpected exception, 2 if the front end is missing, 3 if WebUI cannot
 *         open a window.
 */
int main(int argc, char** argv) {
    try {
        const auto appDir = findFrontend();
        if (appDir.empty()) {
            std::cerr << "Frontend non trovato. Cercato in ./web, nella directory di build e in "
                      << DISKUTILITIES_INSTALL_WEB_DIR << '\n';
            return 2;
        }

        std::filesystem::path initialRoot = defaultUserDirectory();
        if (argc > 1 && argv[1] != nullptr && *argv[1] != '\0') initialRoot = argv[1];
        initialRoot = std::filesystem::absolute(initialRoot).lexically_normal();

        diskutilities::web::SnapshotBridge bridge;
        diskutilities::ScanOptions scanOptions;
        diskutilities::Executor* executor = nullptr; // null: the scanner uses its private std pool
#if defined(DISKUTILITIES_HAS_TK4CPP)
        // Declared before ScanController so it outlives every scan task.
        diskutilities::tk4cpp::Tk4cppExecutor tk4Executor;
        scanOptions.maxNodes = diskutilities::tk4cpp::recommendedNodeLimit();
        executor = &tk4Executor;
#endif
        diskutilities::web::ScanController scans(bridge, scanOptions, executor);
        diskutilities::web::SearchController searches(executor);
        webui::window window;
        diskutilities::web::WebUiAdapter adapter(window, bridge, scans, searches, initialRoot);
        adapter.bind();
        // Handlers are quick and thread-safe; do not serialise UI events behind each other.
        window.set_event_blocking(false);
        if (!window.show(appDir.string())) {
            std::cerr << "WebUI non è riuscita ad aprire la finestra. Verificare la presenza di un browser supportato.\n";
            return 3;
        }
        webui::wait();
        scans.cancel();
        searches.cancel();
        scans.wait();
        searches.wait();
        webui::exit();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "DiskUtilities: " << ex.what() << '\n';
        return 1;
    }
}
