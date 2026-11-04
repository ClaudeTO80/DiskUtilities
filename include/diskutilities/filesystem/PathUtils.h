#pragma once

/**
 * @file PathUtils.h
 * @brief UTF-8 <-> std::filesystem::path helpers shared by every layer.
 * @details File names on POSIX are arbitrary byte strings. Everything that leaves the native layer (JSON for
 *          the browser) must be valid UTF-8, so names are repaired once, at scan time, with toValidUtf8().
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace diskutilities {

/** @brief Convert a filesystem path to UTF-8 with generic ('/') separators. */
inline std::string pathToUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

/** @brief Interpret UI-provided path bytes as UTF-8 on every supported platform. */
inline std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) encoded.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path(encoded);
}

/**
 * @brief Length in bytes of the well-formed UTF-8 sequence starting at @p s[i].
 * @return 1..4 for a valid sequence (overlong forms, surrogates and code points
 *         above U+10FFFF are rejected), 0 if the bytes are not valid UTF-8.
 */
inline std::size_t validUtf8SequenceLength(std::string_view s, std::size_t i) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) return 1;
    const auto continuation = [&](std::size_t k) { return k < s.size() && (byte(k) & 0xC0) == 0x80; };
    if (lead >= 0xC2 && lead <= 0xDF) return continuation(i + 1) ? 2 : 0;
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (!continuation(i + 1) || !continuation(i + 2)) return 0;
        const unsigned char second = byte(i + 1);
        if (lead == 0xE0 && second < 0xA0) return 0; // overlong
        if (lead == 0xED && second > 0x9F) return 0; // UTF-16 surrogates
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (!continuation(i + 1) || !continuation(i + 2) || !continuation(i + 3)) return 0;
        const unsigned char second = byte(i + 1);
        if (lead == 0xF0 && second < 0x90) return 0; // overlong
        if (lead == 0xF4 && second > 0x8F) return 0; // > U+10FFFF
        return 4;
    }
    return 0;
}

/**
 * @brief Return @p value with every ill-formed byte replaced by U+FFFD.
 * @details POSIX file names are arbitrary bytes, but JSON and the browser need valid
 * UTF-8. Sanitising once at scan time keeps names and paths consistent across
 * every later lookup.
 */
inline std::string toValidUtf8(std::string value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const std::size_t length = validUtf8SequenceLength(value, i);
        if (length == 0) break;
        i += length;
    }
    if (i == value.size()) return value; // common case: already valid

    std::string repaired(value, 0, i);
    repaired.reserve(value.size() + 8);
    while (i < value.size()) {
        const std::size_t length = validUtf8SequenceLength(value, i);
        if (length == 0) {
            repaired += "\xEF\xBF\xBD";
            ++i;
        } else {
            repaired.append(value, i, length);
            i += length;
        }
    }
    return repaired;
}

/**
 * @brief The extension of a file name including the dot, as a view into @p name.
 * @details Single definition shared by Snapshot and the live statistics of the scanner so that both always
 * agree. A leading dot (".bashrc") or a trailing dot ("name.") does not start an extension.
 * @param name File name without directory (UTF-8).
 * @return The extension (case preserved), or an empty view if there is none.
 */
[[nodiscard]] inline std::string_view rawExtension(std::string_view name) noexcept {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return {};
    return name.substr(dot);
}

/** @brief ASCII-only lower-casing of one byte (locale independent, safe on UTF-8 sequences). */
[[nodiscard]] constexpr char asciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace diskutilities
