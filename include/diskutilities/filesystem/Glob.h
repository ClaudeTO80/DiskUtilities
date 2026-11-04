#pragma once

/**
 * @file Glob.h
 * @brief Shell-style wildcard matching for file names.
 * @details Supported syntax: `*` (any sequence, including empty), `?` (exactly one UTF-8 code point),
 *          `[abc]`, `[a-z]` and the negations `[!abc]` / `[^abc]` (class members are single bytes, so
 *          ranges are meaningful for ASCII), and `\` to escape the next character. A `[` without a closing
 *          `]` is matched literally. Matching is case sensitive unless requested; case folding is ASCII
 *          only (non-ASCII letters compare by their bytes). The algorithm is iterative with a single
 *          backtrack point (O(pattern * text) worst case, never exponential), so hostile patterns cannot
 *          stall a search. Path separators get no special treatment: patterns are matched against a bare
 *          file name.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/filesystem/PathUtils.h"

#include <cstddef>
#include <string_view>

namespace diskutilities {

namespace detail {

/** @brief Length of the character at @p s[i]: a whole UTF-8 sequence, or 1 byte when ill-formed. */
inline std::size_t glyphLength(std::string_view s, std::size_t i) noexcept {
    const std::size_t n = validUtf8SequenceLength(s, i);
    return n == 0 ? 1 : n;
}

/**
 * @brief Match one pattern element at @p p against the byte(s) at @p t.
 * @param pattern       Whole wildcard pattern.
 * @param text          Whole candidate text; @p t is always a valid position inside it.
 * @param caseSensitive False folds ASCII letters on both sides.
 * @param[in,out] p Pattern position; advanced past the element on return (also when it does not match).
 * @param[in,out] t Text position; advanced past the consumed character on a match.
 * @return True if the element matched.
 */
inline bool globElement(std::string_view pattern, std::size_t& p, std::string_view text, std::size_t& t,
                        bool caseSensitive) noexcept {
    const auto fold = [&](char c) { return caseSensitive ? c : asciiLower(c); };
    const char pc = pattern[p];
    if (pc == '?') {
        ++p;
        t += glyphLength(text, t);
        return true;
    }
    if (pc == '[') {
        // Find the closing bracket: a ']' directly after '[' or '[!' belongs to the class.
        std::size_t q = p + 1;
        bool negate = false;
        if (q < pattern.size() && (pattern[q] == '!' || pattern[q] == '^')) { negate = true; ++q; }
        const std::size_t first = q;
        while (q < pattern.size() && (pattern[q] != ']' || q == first)) ++q;
        if (q < pattern.size()) {
            const char c = fold(text[t]);
            bool in = false;
            for (std::size_t k = first; k < q; ++k) {
                if (k + 2 < q && pattern[k + 1] == '-') {
                    if (c >= fold(pattern[k]) && c <= fold(pattern[k + 2])) in = true;
                    k += 2;
                } else if (c == fold(pattern[k])) {
                    in = true;
                }
            }
            p = q + 1;
            if (in == negate) return false;
            ++t;
            return true;
        }
        // No closing bracket: literal '['.
    }
    std::size_t at = p;
    if (pc == '\\' && p + 1 < pattern.size()) at = p + 1;
    p = at + 1;
    if (fold(pattern[at]) != fold(text[t])) return false;
    ++t;
    return true;
}

} // namespace detail

/**
 * @brief Test whether @p text matches the wildcard @p pattern as a whole.
 * @param pattern       Wildcard expression (see the file documentation); an empty pattern matches only "".
 * @param text          Candidate string, normally a file name.
 * @param caseSensitive False folds ASCII letters on both sides.
 * @return True on a full match.
 */
[[nodiscard]] inline bool globMatch(std::string_view pattern, std::string_view text, bool caseSensitive = true) noexcept {
    std::size_t p = 0, t = 0;
    std::size_t starP = std::string_view::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = ++p;
            starT = t;
            continue;
        }
        if (p < pattern.size()) {
            std::size_t np = p, nt = t;
            if (detail::globElement(pattern, np, text, nt, caseSensitive)) {
                p = np;
                t = nt;
                continue;
            }
        }
        if (starP == std::string_view::npos) return false;
        // Retry with the star swallowing one more character.
        starT += detail::glyphLength(text, starT);
        p = starP;
        t = starT;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

/**
 * @brief Case-aware substring test.
 * @param haystack      Text to search.
 * @param needle        Fragment to look for; an empty needle always matches.
 * @param caseSensitive False folds ASCII letters.
 */
[[nodiscard]] inline bool containsText(std::string_view haystack, std::string_view needle, bool caseSensitive) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    if (caseSensitive) return haystack.find(needle) != std::string_view::npos;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && asciiLower(haystack[i + k]) == asciiLower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

} // namespace diskutilities
