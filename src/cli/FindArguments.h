#pragma once

/**
 * @file FindArguments.h
 * @brief Command-line parsing for `diskutilities-cli find`.
 * @details Separate from main.cpp so that size and date parsing can be unit tested. Pure functions, no I/O.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FileSearcher.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace diskutilities::cli {

/** @brief How matches are printed. */
enum class FindFormat { Paths, Long, Json };

/** @brief Parsed `find` command line. */
struct FindArguments {
    std::filesystem::path root;       ///< Directory to search.
    SearchCriteria criteria;          ///< Filters.
    SearchOptions options;            ///< Result cap, threads, mount handling.
    FindFormat format = FindFormat::Paths; ///< Output format.
    bool nulSeparated = false;        ///< Paths format only: separate with NUL instead of newline (for xargs -0).
    bool help = false;                ///< `--help` was given.
};

/**
 * @brief Parse a size such as `500`, `10k`, `1.5MB`, `2GiB`.
 * @details No suffix or `B` = bytes; `k`/`kB`/`M`/`MB`/`G`/`GB`/`T`/`TB` are powers of 1000 (the units the UI
 *          shows); `KiB`/`MiB`/`GiB`/`TiB` are powers of 1024. Letters are case-insensitive. Decimals are rounded to
 *          the nearest byte.
 * @return The byte count, or std::nullopt if @p text is malformed, negative or does not fit in 64 bits.
 */
[[nodiscard]] std::optional<std::uint64_t> parseSize(std::string_view text);

/**
 * @brief Parse a point in time into Unix seconds.
 * @details Accepted forms: a relative age `<N><unit>` with unit s, m, h, d or w meaning "N units before @p now";
 *          `YYYY-MM-DD`; `YYYY-MM-DDTHH:MM`; `YYYY-MM-DDTHH:MM:SS` (a space may replace the `T`). Absolute forms are
 *          local time. A date without a time is the start of that day, or its last second when @p endOfDay is set
 *          (used for "before" bounds, so that `--modified-before 2026-03-31` includes the whole day).
 * @param text     Text to parse.
 * @param endOfDay See above.
 * @param now      Reference for relative forms.
 */
[[nodiscard]] std::optional<std::int64_t> parseWhen(std::string_view text, bool endOfDay,
                                                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

/**
 * @brief Parse the arguments that follow the word `find`.
 * @return The parsed command, or a message (Italian) describing the problem.
 */
[[nodiscard]] std::expected<FindArguments, std::string> parseFindArguments(const std::vector<std::string>& args);

/** @brief Usage text of the `find` command. */
[[nodiscard]] std::string findUsage();

} // namespace diskutilities::cli
