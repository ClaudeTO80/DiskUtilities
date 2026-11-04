/**
 * @file path_tests.cpp
 * @brief Tests for the UTF-8 path helpers.
 * @details Covers round trips and the repair of malformed byte sequences.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "TestSupport.h"

#include "diskutilities/filesystem/PathUtils.h"

using namespace diskutilities;

void validStringsPassThrough() {
    CHECK_EQ(toValidUtf8("hello.txt"), std::string("hello.txt"));
    CHECK_EQ(toValidUtf8(""), std::string());
    CHECK_EQ(toValidUtf8("caf\xC3\xA9"), std::string("caf\xC3\xA9"));          // é
    CHECK_EQ(toValidUtf8("\xE2\x82\xAC"), std::string("\xE2\x82\xAC"));        // €
    CHECK_EQ(toValidUtf8("\xF0\x9F\x98\x80"), std::string("\xF0\x9F\x98\x80")); // U+1F600
}

void invalidBytesAreReplaced() {
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK_EQ(toValidUtf8("bad\xFF.txt"), "bad" + fffd + ".txt");
    CHECK_EQ(toValidUtf8("\xC3"), fffd);                       // truncated 2-byte sequence
    CHECK_EQ(toValidUtf8("\xE2\x82"), fffd + fffd);            // truncated 3-byte sequence
    CHECK_EQ(toValidUtf8("\xC0\x80"), fffd + fffd);            // overlong NUL
    CHECK_EQ(toValidUtf8("\xED\xA0\x80"), fffd + fffd + fffd); // UTF-16 surrogate
    CHECK_EQ(toValidUtf8("\xF4\x90\x80\x80"), fffd + fffd + fffd + fffd); // > U+10FFFF
    CHECK_EQ(toValidUtf8("a\x80z"), "a" + fffd + "z");         // stray continuation byte
}

void roundTripsUtf8Paths() {
    const auto path = pathFromUtf8("/tmp/caf\xC3\xA9/\xE2\x82\xAC.txt");
    CHECK_EQ(pathToUtf8(path), std::string("/tmp/caf\xC3\xA9/\xE2\x82\xAC.txt"));
}

int main() {
    RUN(validStringsPassThrough);
    RUN(invalidBytesAreReplaced);
    RUN(roundTripsUtf8Paths);
    return testReport("path_tests");
}
