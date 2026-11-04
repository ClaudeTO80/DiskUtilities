/**
 * @file main.cpp
 * @brief DiskUtilities command line: disk usage report and file search.
 * @details `diskutilities-cli <dir>` prints a JSON usage report; `diskutilities-cli find <dir> ...` searches files.
 *          Both use exactly the same scanner, searcher, snapshot and DTO serialization as the GUI. Progress goes to
 *          stderr, results to stdout. SIGINT/SIGTERM cancel the running operation cleanly.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "FindArguments.h"
#include "JsonSerialization.h"

#include "diskutilities/analysis/Snapshot.h"
#include "diskutilities/filesystem/DirectoryScanner.h"
#include "diskutilities/filesystem/FileSearcher.h"
#include "diskutilities/filesystem/PathUtils.h"
#include "diskutilities/web/SnapshotBridge.h"

#include <json/json.h>

#if defined(DISKUTILITIES_HAS_TK4CPP)
#include "diskutilities/tk4cpp/Tk4cppSupport.h"
#if __has_include(<TK4CPP/Utility/SizeFormat.hpp>)
#include <TK4CPP/Utility/SizeFormat.hpp>
#endif
#endif

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace diskutilities;

std::atomic<bool> g_interrupted{false};

extern "C" void onSignal(int) { g_interrupted.store(true); }

struct CliOptions {
    std::filesystem::path root;
    std::size_t top = 100;
    std::size_t treeDepth = 4;
    ScanOptions scan;
    bool help = false;
};

void printUsage(std::ostream& out) {
    out << "DiskUtilities CLI - scansione filesystem in sola lettura\n\n"
        << "Uso: diskutilities-cli [opzioni] <directory>\n"
        << "     diskutilities-cli find [opzioni] <directory>   (ricerca file: vedi find --help)\n\n"
        << "Opzioni:\n"
        << "  --top N            numero di file più grandi nel report (0..10000, default 100)\n"
        << "  --tree-depth N     livelli di directory nel report (0..64, default 4)\n"
        << "  --threads N        thread di scansione (1..64, default automatico)\n"
        << "  --one-file-system  non attraversare i punti di mount (solo POSIX)\n"
        << "  -h, --help         mostra questo aiuto\n\n"
        << "Stampa un report JSON su stdout; avanzamento ed errori diagnostici su stderr.\n";
}

std::optional<std::size_t> parseCount(std::string_view text, std::size_t maximum) {
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value > maximum) return std::nullopt;
    return value;
}

std::expected<CliOptions, std::string> parseArguments(const std::vector<std::string>& args) {
    CliOptions options;
    const auto needValue = [&](std::size_t& i, std::string_view name) -> std::expected<std::string_view, std::string> {
        if (++i >= args.size()) return std::unexpected("Manca il valore di " + std::string(name) + ".");
        return std::string_view(args[i]);
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "--one-file-system") {
            options.scan.stayOnFileSystem = true;
        } else if (arg == "--top" || arg == "--tree-depth" || arg == "--threads") {
            const auto value = needValue(i, arg);
            if (!value) return std::unexpected(value.error());
            const std::size_t maximum = arg == "--top" ? 10000 : (arg == "--tree-depth" ? 64 : 64);
            const auto parsed = parseCount(*value, maximum);
            if (!parsed || (arg == "--threads" && *parsed == 0))
                return std::unexpected("Valore non valido per " + arg + ": " + std::string(*value));
            if (arg == "--top") options.top = *parsed;
            else if (arg == "--tree-depth") options.treeDepth = *parsed;
            else options.scan.threads = static_cast<unsigned>(*parsed);
        } else if (!arg.empty() && arg[0] == '-') {
            return std::unexpected("Opzione sconosciuta: " + arg);
        } else if (options.root.empty()) {
            options.root = pathFromUtf8(arg);
        } else {
            return std::unexpected("È consentita una sola directory.");
        }
    }
    if (!options.help && options.root.empty()) return std::unexpected("Manca la directory da analizzare.");
    return options;
}

/**
 * Human readable size in decimal units, matching the UI and the `--min-size` suffixes.
 * With TK4CPP this is formatSize(bytes, SizeUnits::Decimal); FolderHandler::formatSize stays 1024-based
 * ("MB" label) and would disagree with what `--min-size 1MB` (10^6) selects.
 */
#if defined(DISKUTILITIES_HAS_TK4CPP) && __has_include(<TK4CPP/Utility/SizeFormat.hpp>)
std::string humanSize(std::uint64_t bytes) { return TK4CPP::Utility::formatSize(bytes, TK4CPP::Utility::SizeUnits::Decimal); }
#else
std::string humanSize(std::uint64_t bytes) {
    static constexpr const char* units[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000 && unit < 5) { value /= 1000; ++unit; }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, unit == 0 ? "%.0f %s" : "%.2f %s", value, units[unit]);
    return buffer;
}
#endif

std::string localTime(std::int64_t seconds) {
    if (seconds == kNoTimestamp) return "-";
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) return "-";
#else
    if (localtime_r(&t, &tm) == nullptr) return "-";
#endif
    char buffer[32];
    return std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &tm) == 0 ? "-" : buffer;
}

int runFind(const std::vector<std::string>& args) {
    const auto parsed = cli::parseFindArguments(args);
    if (!parsed) {
        std::cerr << "Errore: " << parsed.error() << "\n\n" << cli::findUsage();
        return 1;
    }
    if (parsed->help) {
        std::cout << cli::findUsage();
        return 0;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::error_code ec;
    const auto root = std::filesystem::absolute(parsed->root, ec).lexically_normal();
    if (ec) {
        std::cerr << "Errore: impossibile risolvere il percorso: " << ec.message() << '\n';
        return 2;
    }

    SearchOptions options = parsed->options;
    Executor* executor = nullptr;
#if defined(DISKUTILITIES_HAS_TK4CPP)
    tk4cpp::Tk4cppExecutor tk4Executor(options.threads);
    executor = &tk4Executor;
#endif
    std::stop_source stop;
    const bool streaming = parsed->format != cli::FindFormat::Json;
    const char separator = parsed->nulSeparated ? '\0' : '\n';
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t lastReported = 0;

    // Plain and long listings stream matches as they are found, so a long search shows output immediately.
    // (Streaming order is arrival order; the JSON report is sorted by path.)
    const auto print = [&](const SearchHit& hit) {
        if (parsed->format == cli::FindFormat::Long) {
            std::cout << (hit.type == EntryType::Directory ? "-" : humanSize(hit.size)) << '\t' << localTime(hit.modified) << '\t';
        }
        std::cout << hit.path << separator;
    };
    FileSearcher searcher;
    auto result = searcher.search(
        root, parsed->criteria, options,
        [&](const SearchProgress& progress) {
            if (g_interrupted.load()) stop.request_stop();
            if (progress.currentPath.empty() || progress.examined < lastReported + 20000) return;
            lastReported = progress.examined;
            std::cerr << "Esaminati: " << progress.examined << " | trovati: " << progress.matches << " | " << progress.currentPath << '\n';
        },
        streaming ? FileSearcher::HitsCallback([&](std::span<const SearchHit> batch) { for (const auto& hit : batch) print(hit); })
                  : FileSearcher::HitsCallback{},
        stop.get_token(), executor);
    if (!result) {
        std::cout.flush();
        std::cerr << "Errore durante la ricerca: " << result.error().message << '\n';
        switch (result.error().code) {
            case SearchError::Code::InvalidPath:
            case SearchError::Code::NotADirectory:
            case SearchError::Code::InvalidCriteria: return 2;
            case SearchError::Code::Cancelled: return 130;
            default: return 3;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    const std::uint64_t matchCount = result->hits.size();

    if (parsed->format == cli::FindFormat::Json) {
        Json::Value report(Json::objectValue);
        report["root"] = pathToUtf8(root);
        report["matches"] = Json::UInt64(result->hits.size());
        report["truncated"] = result->truncated;
        report["examined"] = Json::UInt64(result->examined);
        report["directories"] = Json::UInt64(result->directories);
        report["inaccessible"] = Json::UInt64(result->inaccessible);
        report["creationUnavailable"] = Json::UInt64(result->creationUnavailable);
        report["elapsedMs"] = Json::UInt64(static_cast<std::uint64_t>(elapsed.count()));
        web::SearchPage page;
        page.total = result->hits.size();
        page.hits = std::move(result->hits);
        report["hits"] = web::toJson(page)["hits"];
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        writer["commentStyle"] = "None";
        writer["emitUTF8"] = true;
        std::cout << Json::writeString(writer, report) << '\n';
    }
    std::cerr << "Ricerca completata in " << elapsed.count() << " ms. Esaminati: " << result->examined
              << ", trovati: " << matchCount << ".\n";
    if (result->truncated) std::cerr << "Attenzione: raggiunto il limite di risultati (--max-results): l'elenco è parziale.\n";
    if (result->inaccessible) std::cerr << "Attenzione: " << result->inaccessible << " elementi non accessibili.\n";
    if (result->creationUnavailable)
        std::cerr << "Attenzione: data di creazione non disponibile per " << result->creationUnavailable
                  << " elementi (non supportata dal sistema o dal file system): esclusi.\n";
    return 0;
}

int run(const std::vector<std::string>& args) {
    if (!args.empty() && args.front() == "find") return runFind(std::vector<std::string>(args.begin() + 1, args.end()));
    const auto parsed = parseArguments(args);
    if (!parsed) {
        std::cerr << "Errore: " << parsed.error() << "\n\n";
        printUsage(std::cerr);
        return 1;
    }
    if (parsed->help) {
        printUsage(std::cout);
        return 0;
    }

    std::error_code ec;
    const auto root = std::filesystem::absolute(parsed->root, ec).lexically_normal();
    if (ec) {
        std::cerr << "Errore: impossibile risolvere il percorso: " << ec.message() << '\n';
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cerr << "Scansione: " << pathToUtf8(root) << '\n';
    ScanOptions scanOptions = parsed->scan;
    Executor* executor = nullptr; // null: the scanner uses its private std pool
#if defined(DISKUTILITIES_HAS_TK4CPP)
    // Backed by TK4CPP::TP::ThreadPool; --threads overrides the Utility::System based default.
    tk4cpp::Tk4cppExecutor tk4Executor(scanOptions.threads);
    scanOptions.maxNodes = tk4cpp::recommendedNodeLimit();
    executor = &tk4Executor;
#endif
    std::stop_source stop;
    std::uint64_t lastReported = 0;
    const auto started = std::chrono::steady_clock::now();
    DirectoryScanner scanner;
    auto tree = scanner.scan(
        root, scanOptions,
        [&](const ScanProgress& progress) {
            if (g_interrupted.load()) stop.request_stop();
            if (progress.currentPath.empty() || progress.entriesVisited < lastReported + 5000) return;
            lastReported = progress.entriesVisited;
            std::cerr << "Visitati: " << progress.entriesVisited << " | file: " << progress.filesFound
                      << " | directory: " << progress.directoriesFound
                      << " | non accessibili: " << progress.inaccessibleFound << " | " << progress.currentPath << '\n';
        },
        stop.get_token(), executor);
    if (!tree) {
        std::cerr << "Errore durante la scansione: " << tree.error().message << '\n';
        switch (tree.error().code) {
            case ScanError::Code::InvalidPath:
            case ScanError::Code::NotADirectory: return 2;
            case ScanError::Code::Cancelled: return 130;
            default: return 3;
        }
    }

    VolumeInfo volume;
    const auto space = std::filesystem::space(root, ec);
    if (!ec) volume = {space.capacity, space.free, space.available, true};
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    web::SnapshotBridge bridge;
    bridge.publish(Snapshot::create(std::move(*tree), volume, elapsed));

    Json::Value report(Json::objectValue);
    report["status"] = "completed";
    report["root"] = pathToUtf8(root);
    const auto dashboard = bridge.getDashboard(root);
    report["dashboard"] = web::toJson(dashboard);
    report["largestFiles"] = web::toJson(bridge.getLargestFiles(root, parsed->top));
    report["extensions"] = web::toJson(bridge.getExtensionStatistics(root));
    report["directoryTree"] = web::toJson(bridge.getDirectoryTree(root, parsed->treeDepth));
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    writer["commentStyle"] = "None";
    writer["emitUTF8"] = true;
    std::cout << Json::writeString(writer, report) << '\n';
    std::cerr << "Scansione completata in " << elapsed.count() << " ms. File: " << dashboard.fileCount
              << ", directory: " << dashboard.directoryCount << ", dimensione logica: " << dashboard.totalBytes << " byte"
#if defined(DISKUTILITIES_HAS_TK4CPP)
              << " (" << tk4cpp::formatBytes(dashboard.totalBytes) << ")"
#endif
              << ".\n";
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(pathToUtf8(std::filesystem::path(argv[i] ? argv[i] : L"")));
    return run(args);
}
#else
/**
 * @brief Program entry point of the CLI.
 * @param argc Argument count.
 * @param argv Arguments; see `--help`.
 * @return 0 on success, 1 on a usage error, 2 on an invalid path, 3 on an internal error, 130 when
 *         interrupted by a signal.
 */
int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    return run(args);
}
#endif
