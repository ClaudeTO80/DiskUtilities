/**
 * @file FindArguments.cpp
 * @brief Command-line parsing for `diskutilities-cli find`.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "FindArguments.h"

#include "diskutilities/filesystem/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <ctime>

// TK4CPP >= the search-support release ships the same parsers; use them when present so that the
// CLI, the TK4CPP filters and every other TK4CPP consumer agree. The bundled copies below are the fallback.
#if defined(DISKUTILITIES_HAS_TK4CPP) && __has_include(<TK4CPP/Utility/SizeFormat.hpp>) && __has_include(<TK4CPP/Utility/TimeParse.hpp>)
#define DISKUTILITIES_USE_TK4CPP_PARSERS 1
#include <TK4CPP/Utility/SizeFormat.hpp>
#include <TK4CPP/Utility/TimeParse.hpp>
#endif

namespace diskutilities::cli {

namespace {

[[maybe_unused]] std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[maybe_unused]] bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

#if defined(DISKUTILITIES_USE_TK4CPP_PARSERS)
std::optional<std::uint64_t> parseSize(std::string_view text) { return TK4CPP::Utility::parseSize(text); }

std::optional<std::int64_t> parseWhen(std::string_view text, bool endOfDay, std::chrono::system_clock::time_point now) {
    return TK4CPP::Utility::parseTimestamp(text, endOfDay, now);
}
#else
std::optional<std::uint64_t> parseSize(std::string_view text) {
    std::size_t split = 0;
    while (split < text.size() && ((text[split] >= '0' && text[split] <= '9') || text[split] == '.')) ++split;
    if (split == 0) return std::nullopt;
    const std::string number(text.substr(0, split));
    const std::string unit = lower(text.substr(split));
    if (std::count(number.begin(), number.end(), '.') > 1) return std::nullopt;

    double multiplier = 1;
    if (unit.empty() || unit == "b") multiplier = 1;
    else if (unit == "k" || unit == "kb") multiplier = 1e3;
    else if (unit == "m" || unit == "mb") multiplier = 1e6;
    else if (unit == "g" || unit == "gb") multiplier = 1e9;
    else if (unit == "t" || unit == "tb") multiplier = 1e12;
    else if (unit == "kib") multiplier = 1024.0;
    else if (unit == "mib") multiplier = 1024.0 * 1024;
    else if (unit == "gib") multiplier = 1024.0 * 1024 * 1024;
    else if (unit == "tib") multiplier = 1024.0 * 1024 * 1024 * 1024;
    else return std::nullopt;

    double value = 0;
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (ec != std::errc{} || end != number.data() + number.size()) return std::nullopt;
    const double bytes = std::round(value * multiplier);
    if (!(bytes >= 0) || bytes >= 18446744073709551615.0) return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

std::optional<std::int64_t> parseWhen(std::string_view text, bool endOfDay, std::chrono::system_clock::time_point now) {
    using namespace std::chrono;
    if (text.empty()) return std::nullopt;

    // Relative age: "7d", "36h", "2w".
    if (const char unit = text.back(); std::string_view("smhdw").find(unit) != std::string_view::npos && text.size() > 1) {
        const auto digits = text.substr(0, text.size() - 1);
        if (allDigits(digits)) {
            std::int64_t n = 0;
            const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
            if (ec != std::errc{} || end != digits.data() + digits.size() || n > 100000000) return std::nullopt;
            const std::int64_t seconds = unit == 's' ? 1 : unit == 'm' ? 60 : unit == 'h' ? 3600 : unit == 'd' ? 86400 : 604800;
            return duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() - n * seconds;
        }
    }

    // Absolute local time: "YYYY-MM-DD", optionally followed by "T" or " " and "hh:mm" or "hh:mm:ss".
    // std::from_chars replaces sscanf (deprecated by MSVC, C4996): each field is read with a bounded number of digits.
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    bool hasTime = false;
    std::size_t pos = 0;
    const auto number = [&](std::size_t maxDigits) -> std::optional<int> {
        std::size_t stop = pos;
        while (stop < text.size() && stop - pos < maxDigits && text[stop] >= '0' && text[stop] <= '9') ++stop;
        if (stop == pos) return std::nullopt;
        int value = 0;
        const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + stop, value);
        if (ec != std::errc{} || ptr != text.data() + stop) return std::nullopt;
        pos = stop;
        return value;
    };
    const auto expect = [&](char c) {
        if (pos < text.size() && text[pos] == c) { ++pos; return true; }
        return false;
    };
    const auto yearValue = number(4);
    if (!yearValue || !expect('-')) return std::nullopt;
    year = *yearValue;
    const auto monthValue = number(2);
    if (!monthValue || !expect('-')) return std::nullopt;
    month = *monthValue;
    const auto dayValue = number(2);
    if (!dayValue) return std::nullopt;
    day = *dayValue;
    if (pos < text.size()) {
        if (!expect('T') && !expect(' ')) return std::nullopt;
        const auto hourValue = number(2);
        if (!hourValue || !expect(':')) return std::nullopt;
        const auto minuteValue = number(2);
        if (!minuteValue) return std::nullopt;
        hour = *hourValue;
        minute = *minuteValue;
        if (pos < text.size()) {
            if (!expect(':')) return std::nullopt;
            const auto secondValue = number(2);
            if (!secondValue) return std::nullopt;
            second = *secondValue;
        }
        if (pos != text.size()) return std::nullopt;
        hasTime = true;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60 || year < 1) return std::nullopt;
    if (!hasTime && endOfDay) { hour = 23; minute = 59; second = 59; }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    // mktime silently normalises 31 February to 3 March: reject such dates.
    if (tm.tm_mday != day || tm.tm_mon != month - 1) return std::nullopt;
    return static_cast<std::int64_t>(t);
}
#endif

std::string findUsage() {
    return
        "Uso: diskutilities-cli find [opzioni] <directory>\n\n"
        "Cerca file e cartelle (sola lettura, i symlink non vengono seguiti). Tutti i filtri indicati devono valere.\n\n"
        "Nome:\n"
        "  --name TESTO          il nome contiene TESTO\n"
        "  --pattern GLOB        il nome corrisponde al pattern (* ? [a-z] [!x]); ripetibile, basta uno\n"
        "  --exclude GLOB        scarta i nomi che corrispondono; ripetibile\n"
        "  --case-sensitive      distingue maiuscole e minuscole (default: no, solo ASCII)\n"
        "Tipo:\n"
        "  --type f|d|any        file (default), cartelle oppure entrambi\n"
        "  --no-hidden           ignora i nomi che iniziano con '.'\n"
        "Dimensione (byte, oppure con suffisso k/M/G/T = 1000, KiB/MiB/GiB/TiB = 1024):\n"
        "  --min-size N   --max-size N\n"
        "Date (locali: AAAA-MM-GG, AAAA-MM-GGTHH:MM[:SS], oppure età relativa 30m 12h 7d 2w):\n"
        "  --modified-after D   --modified-before D\n"
        "  --created-after D    --created-before D     (richiede il supporto del sistema: Linux/macOS)\n"
        "Esecuzione e output:\n"
        "  --max-results N       fermati dopo N risultati (1..100000, default 10000)\n"
        "  --threads N           thread di ricerca (1..64, default automatico)\n"
        "  --one-file-system     non attraversare i punti di mount (solo POSIX)\n"
        "  -l, --long            dimensione, data di modifica e percorso\n"
        "  --json                report JSON\n"
        "  -0, --print0          percorsi separati da NUL (per xargs -0)\n"
        "  -h, --help            mostra questo aiuto\n\n"
        "Codici di uscita: 0 ricerca eseguita (anche senza risultati), 1 uso errato, 2 percorso non valido,\n"
        "3 errore interno, 130 interrotta.\n";
}

std::expected<FindArguments, std::string> parseFindArguments(const std::vector<std::string>& args) {
    FindArguments out;
    const auto value = [&](std::size_t& i) -> std::expected<std::string, std::string> {
        if (i + 1 >= args.size()) return std::unexpected("Manca il valore di " + args[i] + ".");
        return args[++i];
    };
    const auto bad = [](const std::string& option, const std::string& text) {
        return std::unexpected("Valore non valido per " + option + ": " + text);
    };
    SearchCriteria& c = out.criteria;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool needsValue = arg == "--name" || arg == "--pattern" || arg == "--exclude" || arg == "--type" ||
            arg == "--min-size" || arg == "--max-size" || arg == "--modified-after" || arg == "--modified-before" ||
            arg == "--created-after" || arg == "--created-before" || arg == "--max-results" || arg == "--threads";
        if (arg == "-h" || arg == "--help") out.help = true;
        else if (arg == "--case-sensitive") c.caseSensitive = true;
        else if (arg == "--no-hidden") c.includeHidden = false;
        else if (arg == "--one-file-system") out.options.stayOnFileSystem = true;
        else if (arg == "-l" || arg == "--long") out.format = FindFormat::Long;
        else if (arg == "--json") out.format = FindFormat::Json;
        else if (arg == "-0" || arg == "--print0") out.nulSeparated = true;
        else if (needsValue) {
            const auto v = value(i);
            if (!v) return std::unexpected(v.error());
            if (arg == "--name") c.nameContains = *v;
            else if (arg == "--pattern") c.patterns.push_back(*v);
            else if (arg == "--exclude") c.excludePatterns.push_back(*v);
            else if (arg == "--type") {
                if (*v == "f" || *v == "file" || *v == "files") c.kind = SearchKind::Files;
                else if (*v == "d" || *v == "dir" || *v == "directories") c.kind = SearchKind::Directories;
                else if (*v == "any") c.kind = SearchKind::Any;
                else return bad(arg, *v);
            } else if (arg == "--min-size" || arg == "--max-size") {
                const auto size = parseSize(*v);
                if (!size) return bad(arg, *v);
                (arg == "--min-size" ? c.minSize : c.maxSize) = *size;
            } else if (arg == "--max-results" || arg == "--threads") {
                std::uint64_t n = 0;
                const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
                const std::uint64_t hi = arg == "--threads" ? 64 : 100000;
                if (ec != std::errc{} || end != v->data() + v->size() || n < 1 || n > hi) return bad(arg, *v);
                if (arg == "--threads") out.options.threads = static_cast<unsigned>(n);
                else out.options.maxResults = n;
            } else {
                const bool before = arg.ends_with("-before");
                const auto when = parseWhen(*v, before);
                if (!when) return bad(arg, *v);
                if (arg == "--modified-after") c.modifiedAfter = when;
                else if (arg == "--modified-before") c.modifiedBefore = when;
                else if (arg == "--created-after") c.createdAfter = when;
                else c.createdBefore = when;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            return std::unexpected("Opzione sconosciuta: " + arg);
        } else if (out.root.empty()) {
            out.root = pathFromUtf8(arg);
        } else {
            return std::unexpected("È consentita una sola directory.");
        }
    }
    if (out.help) return out;
    if (out.root.empty()) return std::unexpected("Manca la directory in cui cercare.");
    if (out.nulSeparated && out.format != FindFormat::Paths) return std::unexpected("--print0 si usa solo con l'elenco semplice dei percorsi.");
    if (auto invalid = FileSearcher::validate(c)) return std::unexpected(invalid->message);
    if (c.kind == SearchKind::Directories && (c.minSize || c.maxSize))
        return std::unexpected("Le cartelle non hanno una dimensione: --min-size/--max-size valgono solo per i file.");
    return out;
}

} // namespace diskutilities::cli
