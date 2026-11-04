#pragma once

/**
 * @file EntryStat.h
 * @brief Single-call metadata probe for a directory entry.
 * @details Implementation detail of the scanner, exposed in a public header only so that the POSIX and the
 *          portable code paths can both be unit tested on the same platform.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskanalyzer/filesystem/FileTree.h"

#include <cstdint>
#include <filesystem>

namespace diskanalyzer::detail {

/** @brief Metadata of one entry, obtained without following symbolic links. */
struct EntryStat {
    EntryType type = EntryType::Other;      ///< Kind of entry (links are not followed).
    std::uint64_t size = 0;                 ///< Regular files only.
    std::int64_t modified = kNoTimestamp;   ///< Seconds since the Unix epoch.
    std::uint64_t device = 0;               ///< Filesystem id (POSIX only, 0 otherwise).
    bool ok = false;                        ///< False if the metadata could not be read.
};

/**
 * @brief Probe @p entry using the best available strategy.
 * @details On POSIX a single lstat() call yields type, size, mtime and device. The
 * portable variant uses the (on MSVC cached) directory_entry accessors and is
 * always compiled so that it can be tested on every platform.
 */
EntryStat probeEntry(const std::filesystem::directory_entry& entry) noexcept;

/** @brief Portable implementation based on std::filesystem only. */
EntryStat probeEntryPortable(const std::filesystem::directory_entry& entry) noexcept;

/** @brief Convert a file_time_type to Unix seconds. */
std::int64_t toUnixSeconds(std::filesystem::file_time_type time) noexcept;

} // namespace diskanalyzer::detail
