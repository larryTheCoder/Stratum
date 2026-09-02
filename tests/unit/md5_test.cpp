// Stratum — MD5 tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Checked against md5sum, an implementation that is not ours. The inputs
// include the padding boundaries, where a length-dependent bug hides: an
// implementation can be right for every short string and still wrong at 56
// bytes, where the tail no longer fits in one block.

#include "md5_vectors.inc" // NOLINT(misc-include-cleaner) — generated

#include <stratum/hash/md5.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_CASE("md5 matches md5sum", "[hash][md5][vectors]") {
    for (const Md5Vector& vector : kMd5Vectors) {
        CAPTURE(vector.input.size());
        CHECK(stratum::hash::toHex(stratum::hash::md5(vector.input)) == vector.digest);
    }
}

TEST_CASE("the octave salts vanilla uses come out right", "[hash][md5]") {
    // These are the salts the noise octaves are seeded from; a wrong digest
    // here would shift every octave of every noise in the world.
    CHECK(stratum::hash::toHex(stratum::hash::md5("octave_0")) ==
          "d50708086cef4d7c6e1651ecc7f43309");
    CHECK(stratum::hash::toHex(stratum::hash::md5("octave_-7")) ==
          "f11268128982754f257a1d670430b0aa");
    CHECK(stratum::hash::toHex(stratum::hash::md5("octave_-12")) ==
          "b198de63a80126727b84cad43ef7b5a8");
}

TEST_CASE("the digest is exactly 16 bytes, whatever the input", "[hash][md5]") {
    CHECK(stratum::hash::md5("").size() == 16U);
    CHECK(stratum::hash::md5(std::string(10000, 'z')).size() == 16U);
}
