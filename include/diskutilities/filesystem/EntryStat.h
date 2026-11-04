#pragma once

/**
 * @file EntryStat.h
 * @brief Single-call metadata probe for a directory entry.
 * @details Implementation detail of the scanner, exposed in a public header only so that the POSIX and the
 *          portable code paths can both be unit tested on the same platform.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/FileTree.h"

#include <cstdint>
#include <filesystem>

namespace diskutilities::detail {

/** @brief Metadata of one entry, obtained without following symbolic links. */
struct EntryStat {
    EntryType type = EntryType::Other;      ///< Kind of entry (links are not followed).
    std::uint64_t size = 0;                 ///< Regular files only.
    std::int64_t modified = kNoTimestamp;   ///< Seconds since the Unix epoch.
    std::int64_t created = kNoTimestamp;    ///< Creation (birth) time; only filled when requested and supported.
    std::uint64_t device = 0;               ///< Filesystem id (POSIX only, 0 otherwise).
    bool ok = false;                        ///< False if the metadata could not be read.
};

/**
 * @brief Probe @p entry using the best available strategy.
 * @details On POSIX a single lstat() call yields type, size, mtime and device. The
 * portable variant uses the (on MSVC cached) directory_entry accessors and is
 * always compiled so that it can be tested on every platform.
 *
 * @param entry        Entry to inspect.
 * @param withCreation Also fetch the creation (birth) time into EntryStat::created. It stays kNoTimestamp when
 *                     the platform or the filesystem does not record it. Linux uses statx(STATX_BTIME) (st_ctime
 *                     is the inode *change* time there, not the creation time), macOS st_birthtime. Windows and the
 *                     portable variant have no standard API for it and never set it (not implemented yet).
 */
EntryStat probeEntry(const std::filesystem::directory_entry& entry, bool withCreation = false) noexcept;

/** @brief Portable implementation based on std::filesystem only. */
EntryStat probeEntryPortable(const std::filesystem::directory_entry& entry, bool withCreation = false) noexcept;

/** @brief Convert a file_time_type to Unix seconds. */
std::int64_t toUnixSeconds(std::filesystem::file_time_type time) noexcept;

} // namespace diskutilities::detail
