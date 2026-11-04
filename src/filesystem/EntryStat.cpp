/**
 * @file EntryStat.cpp
 * @brief Metadata probes (POSIX lstat and portable std::filesystem variants).
 * @details On POSIX one lstat() call per entry provides type, size, mtime and device id. The portable variant
 *          is always compiled and is selected on other platforms or when DISKUTILITIES_DISABLE_POSIX_STAT is
 *          defined.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/EntryStat.h"

#include <chrono>
#include <system_error>

#if (defined(__unix__) || defined(__APPLE__)) && !defined(DISKUTILITIES_DISABLE_POSIX_STAT)
#define DISKUTILITIES_HAS_POSIX_STAT 1
#include <sys/stat.h>
#endif
#if defined(DISKUTILITIES_HAS_POSIX_STAT) && defined(__linux__) && defined(STATX_BTIME)
#define DISKUTILITIES_HAS_STATX 1
#include <fcntl.h>
#endif

namespace diskutilities::detail {

namespace {

/**
 * Convert a file clock time point to system_clock. Uses clock_cast when the standard
 * library provides it (detected with a requires-expression: __cpp_lib_chrono is not
 * reliable across implementations), and an approximate "now()" offset otherwise.
 */
template <class TimePoint>
auto toSystemTime(TimePoint time) {
    using namespace std::chrono;
    if constexpr (requires { clock_cast<system_clock>(time); }) {
        return clock_cast<system_clock>(time);
    } else {
        return time_point_cast<system_clock::duration>(
            time - TimePoint::clock::now() + system_clock::now());
    }
}

} // namespace

std::int64_t toUnixSeconds(std::filesystem::file_time_type time) noexcept {
    using namespace std::chrono;
    return floor<seconds>(toSystemTime(time).time_since_epoch()).count();
}

EntryStat probeEntryPortable(const std::filesystem::directory_entry& entry, bool /*withCreation*/) noexcept {
    EntryStat result;
    std::error_code ec;
    const auto status = entry.symlink_status(ec);
    if (ec) return result;
    if (std::filesystem::is_symlink(status)) result.type = EntryType::Symlink;
    else if (std::filesystem::is_directory(status)) result.type = EntryType::Directory;
    else if (std::filesystem::is_regular_file(status)) result.type = EntryType::RegularFile;
    else result.type = EntryType::Other;
    result.ok = true;

    if (result.type == EntryType::RegularFile) {
        const auto size = entry.file_size(ec);
        if (ec) result.ok = false;
        else result.size = size;
    }
    if (result.type != EntryType::Symlink && result.type != EntryType::Other) {
        ec.clear();
        const auto time = entry.last_write_time(ec);
        if (ec) result.ok = false;
        else result.modified = toUnixSeconds(time);
    }
    return result;
}

#if defined(DISKUTILITIES_HAS_POSIX_STAT)
EntryStat probeEntry(const std::filesystem::directory_entry& entry, bool withCreation) noexcept {
    EntryStat result;
    struct stat st{};
    if (::lstat(entry.path().c_str(), &st) != 0) return result;
    result.ok = true;
#if defined(__APPLE__)
    if (withCreation) result.created = static_cast<std::int64_t>(st.st_birthtimespec.tv_sec);
#elif defined(DISKUTILITIES_HAS_STATX)
    if (withCreation) {
        // Second syscall, paid only by callers that filter on creation time. Filesystems without btime
        // (or old kernels) leave the mask bit clear: created stays kNoTimestamp.
        struct statx sx{};
        if (::statx(AT_FDCWD, entry.path().c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) == 0 &&
            (sx.stx_mask & STATX_BTIME) != 0)
            result.created = static_cast<std::int64_t>(sx.stx_btime.tv_sec);
    }
#else
    (void)withCreation;
#endif
    result.device = static_cast<std::uint64_t>(st.st_dev);
    if (S_ISLNK(st.st_mode)) {
        result.type = EntryType::Symlink;
    } else if (S_ISDIR(st.st_mode)) {
        result.type = EntryType::Directory;
        result.modified = static_cast<std::int64_t>(st.st_mtime);
    } else if (S_ISREG(st.st_mode)) {
        result.type = EntryType::RegularFile;
        result.size = static_cast<std::uint64_t>(st.st_size);
        result.modified = static_cast<std::int64_t>(st.st_mtime);
    } else {
        result.type = EntryType::Other;
    }
    return result;
}
#else
EntryStat probeEntry(const std::filesystem::directory_entry& entry, bool withCreation) noexcept {
    return probeEntryPortable(entry, withCreation);
}
#endif

} // namespace diskutilities::detail
