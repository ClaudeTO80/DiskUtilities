#pragma once

/**
 * @file TestSupport.h
 * @brief Tiny dependency-free test harness: non-fatal CHECK macros and a temp directory helper.
 * @details Deliberately minimal so that the test suite builds without any third-party framework.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

namespace testing {

template <class T>
concept Streamable = requires(std::ostream& o, const T& v) { o << v; };

/** Print a value if it is streamable, a placeholder otherwise. */
template <class T>
std::string show(const T& value) {
    if constexpr (Streamable<T>) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else {
        return "<unprintable>";
    }
}

/** @brief Global counter of failed checks. */
inline int& failures() { static int count = 0; return count; }
/** @brief Global counter of executed checks. */
inline int& checks() { static int count = 0; return count; }

} // namespace testing

/** @brief Non-fatal assertion: records a failure and continues. @param cond Expression that must be true. */
#define CHECK(cond)                                                                             \
    do {                                                                                        \
        ++testing::checks();                                                                    \
        if (!(cond)) {                                                                          \
            ++testing::failures();                                                              \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << std::endl;  \
        }                                                                                       \
    } while (0)

/** @brief Non-fatal equality assertion that prints both values. @param a Actual value. @param b Expected value. */
#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        ++testing::checks();                                                                    \
        const auto va_ = (a);                                                                   \
        const auto vb_ = (b);                                                                   \
        if (!(va_ == vb_)) {                                                                    \
            ++testing::failures();                                                              \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK_EQ failed: " #a " == " #b      \
                      << "  (" << testing::show(va_) << " vs " << testing::show(vb_) << ")" << std::endl;                     \
        }                                                                                       \
    } while (0)

/** @brief Run one test function and print `ok`/`FAIL`. @param test Name of a `void()` function. */
#define RUN(test)                                                                               \
    do {                                                                                        \
        const int before_ = testing::failures();                                                \
        test();                                                                                 \
        std::cerr << (testing::failures() == before_ ? "  ok    " : "  FAIL  ") << #test << std::endl; \
    } while (0)

/**
 * @brief Print the summary of a suite.
 * @param suite Suite name.
 * @return Process exit code: 0 if every check passed.
 */
inline int testReport(const char* suite) {
    std::cerr << suite << ": " << testing::checks() << " checks, " << testing::failures() << " failures\n";
    return testing::failures() == 0 ? 0 : 1;
}

/** Unique temporary directory removed (recursively) on destruction. */
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = base / ("diskutilities-test-" + std::to_string(rd()) + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                path_ = std::filesystem::canonical(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create temporary directory");
    }
    ~TempDir() {
        std::error_code ec;
        // Restore permissions that tests may have removed, then delete.
        for (auto it = std::filesystem::recursive_directory_iterator(
                 path_, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, ec);
        }
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    /** @brief Absolute, canonical path of the directory. */
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/** Create a file with the given logical size (sparse). */
inline void makeFile(const std::filesystem::path& path, std::uintmax_t size) {
    std::filesystem::create_directories(path.parent_path());
    { std::ofstream out(path, std::ios::binary); }
    std::filesystem::resize_file(path, size);
}
