/**
 * @file glob_tests.cpp
 * @brief Tests for the wildcard matcher and the substring helper.
 * @details Covers *, ?, character classes, escapes, case folding, UTF-8 handling and the absence of
 *          exponential backtracking.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/filesystem/Glob.h"

using namespace diskutilities;

void starAndQuestion() {
    CHECK(globMatch("*", ""));
    CHECK(globMatch("*", "anything"));
    CHECK(globMatch("*.cpp", "main.cpp"));
    CHECK(!globMatch("*.cpp", "main.cpp.bak"));
    CHECK(globMatch("*.c*", "main.cpp"));
    CHECK(globMatch("a*b*c", "aXXbYYc"));
    CHECK(!globMatch("a*b*c", "aXXbYY"));
    CHECK(globMatch("a?c", "abc"));
    CHECK(!globMatch("a?c", "ac"));
    CHECK(!globMatch("a?c", "abbc"));
    CHECK(globMatch("", ""));
    CHECK(!globMatch("", "x"));
    CHECK(!globMatch("x", ""));
    CHECK(globMatch("**a**", "bab"));
}

void classes() {
    CHECK(globMatch("file[0-9].txt", "file7.txt"));
    CHECK(!globMatch("file[0-9].txt", "fileX.txt"));
    CHECK(globMatch("[abc]x", "bx"));
    CHECK(!globMatch("[!abc]x", "bx"));
    CHECK(globMatch("[!abc]x", "dx"));
    CHECK(globMatch("[^abc]x", "dx"));
    CHECK(globMatch("[]]x", "]x"));      // ']' first is a member
    CHECK(globMatch("[a-]x", "-x"));     // trailing '-' is literal
    CHECK(globMatch("a[", "a["));        // unterminated class is literal
    CHECK(globMatch("a[b", "a[b"));
    CHECK(!globMatch("[a-c]", "d"));
    CHECK(globMatch("[a-c]*", "bzzz"));
}

void escapes() {
    CHECK(globMatch("a\\*b", "a*b"));
    CHECK(!globMatch("a\\*b", "aXb"));
    CHECK(globMatch("a\\?", "a?"));
    CHECK(!globMatch("a\\?", "ab"));
    CHECK(globMatch("a\\", "a\\"));      // trailing backslash is literal
}

void caseFolding() {
    CHECK(!globMatch("*.CPP", "main.cpp", true));
    CHECK(globMatch("*.CPP", "main.cpp", false));
    CHECK(globMatch("[A-C]x", "bx", false));
    CHECK(globMatch("[a-c]x", "BX", false));
    CHECK(!globMatch("[!a-c]x", "BX", false));
    // Only ASCII is folded: 'É' and 'é' differ.
    CHECK(!globMatch("\xC3\x89", "\xC3\xA9", false));
}

void utf8QuestionMarkIsOneCodePoint() {
    CHECK(globMatch("caf?", "caf\xC3\xA9"));            // é, two bytes
    CHECK(!globMatch("caf??", "caf\xC3\xA9"));
    CHECK(globMatch("?", "\xE2\x82\xAC"));              // euro sign, three bytes
    CHECK(globMatch("?", "\xF0\x9F\x98\x80"));          // emoji, four bytes
    CHECK(globMatch("*\xC3\xA9", "caf\xC3\xA9"));
    CHECK(globMatch("*?\xC3\xA9", "caf\xC3\xA9"));
    // Ill-formed bytes count as one character each and never crash.
    CHECK(globMatch("a?b", "a\xFF" "b"));
    CHECK(globMatch("*", "\xC3"));
}

void noExponentialBacktracking() {
    const std::string text(5000, 'a');
    const std::string pattern = "a*a*a*a*a*a*a*a*a*a*b";
    const auto begin = std::chrono::steady_clock::now();
    CHECK(!globMatch(pattern, text));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(globMatch("*a*a*a*a*a*a*a*a*a*a", text));
}

void substring() {
    CHECK(containsText("Report-2025.PDF", "port", true));
    CHECK(!containsText("Report-2025.PDF", "pdf", true));
    CHECK(containsText("Report-2025.PDF", "pdf", false));
    CHECK(containsText("abc", "", true));
    CHECK(!containsText("ab", "abc", false));
    CHECK(containsText("abc", "abc", false));
    CHECK(containsText("xxabc", "ABC", false));
}

int main() {
    RUN(starAndQuestion);
    RUN(classes);
    RUN(escapes);
    RUN(caseFolding);
    RUN(utf8QuestionMarkIsOneCodePoint);
    RUN(noExponentialBacktracking);
    RUN(substring);
    return testReport("glob_tests");
}
