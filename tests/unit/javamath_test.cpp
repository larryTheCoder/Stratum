// Stratum — Java-semantics helper tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §5.2 / CLAUDE.md: these helpers are mandatory, and a wrong one shifts
// every downstream seed and coordinate. The tables below are known-answer
// vectors observed from a real JVM (tools/vectors/JavaMathVectors.java), not
// values reasoned out by hand.

#include "javamath_vectors.inc" // NOLINT(misc-include-cleaner) — generated

#include <stratum/javamath.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

namespace jm = stratum::javamath;

namespace {

constexpr std::int32_t kInt32Min = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kInt32Max = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

} // namespace

TEST_CASE("32-bit division matches the JVM", "[javamath][vectors]") {
    for (const DivVector32& v : kDivVectors32) {
        CAPTURE(v.x, v.y);
        CHECK(jm::floorDiv(v.x, v.y) == v.floorDiv);
        CHECK(jm::floorMod(v.x, v.y) == v.floorMod);
        CHECK(jm::truncDiv(v.x, v.y) == v.truncDiv);
        CHECK(jm::remainder(v.x, v.y) == v.remainder);
    }
}

TEST_CASE("64-bit division matches the JVM", "[javamath][vectors]") {
    for (const DivVector64& v : kDivVectors64) {
        CAPTURE(v.x, v.y);
        CHECK(jm::floorDiv(v.x, v.y) == v.floorDiv);
        CHECK(jm::floorMod(v.x, v.y) == v.floorMod);
    }
}

TEST_CASE("32-bit shifts match the JVM, including masked distances", "[javamath][vectors][shift]") {
    for (const ShiftVector32& v : kShiftVectors32) {
        CAPTURE(v.value, v.distance);
        CHECK(jm::shl(v.value, v.distance) == v.shl);
        CHECK(jm::shr(v.value, v.distance) == v.shr);
        CHECK(jm::ushr(v.value, v.distance) == v.ushr);
    }
}

TEST_CASE("64-bit shifts match the JVM, including masked distances", "[javamath][vectors][shift]") {
    for (const ShiftVector64& v : kShiftVectors64) {
        CAPTURE(v.value, v.distance);
        CHECK(jm::shl(v.value, v.distance) == v.shl);
        CHECK(jm::shr(v.value, v.distance) == v.shr);
        CHECK(jm::ushr(v.value, v.distance) == v.ushr);
    }
}

TEST_CASE("double narrowing saturates the way Java does", "[javamath][vectors]") {
    for (const NarrowingVector& v : kNarrowingVectors) {
        CAPTURE(v.value);
        CHECK(jm::doubleToInt(v.value) == v.toInt);
        CHECK(jm::doubleToLong(v.value) == v.toLong);
        CHECK(jm::floorToInt(v.value) == v.floorToInt);
    }
}

// --- The landmines from CLAUDE.md, called out individually so a regression
// --- names the actual failure rather than "vector 137".

TEST_CASE("MIN_VALUE / -1 wraps instead of trapping", "[javamath][landmine]") {
    // Both expressions are undefined behaviour written directly in C++.
    CHECK(jm::floorDiv(kInt32Min, -1) == kInt32Min);
    CHECK(jm::truncDiv(kInt32Min, -1) == kInt32Min);
    CHECK(jm::floorMod(kInt32Min, -1) == 0);
    CHECK(jm::remainder(kInt32Min, -1) == 0);
    CHECK(jm::floorDiv(kInt64Min, INT64_C(-1)) == kInt64Min);
    CHECK(jm::floorMod(kInt64Min, INT64_C(-1)) == 0);
}

TEST_CASE("shift distances are masked, not clamped or undefined", "[javamath][landmine][shift]") {
    // Java: x << 32 is x, x << 33 is x << 1, x << -1 is x << 31.
    CHECK(jm::shl(1, 32) == 1);
    CHECK(jm::shl(1, 33) == 2);
    CHECK(jm::shl(1, -1) == kInt32Min);
    CHECK(jm::ushr(-1, 32) == -1);
    CHECK(jm::ushr(-1, 33) == kInt32Max);
    CHECK(jm::shl(INT64_C(1), 64) == 1);
    CHECK(jm::shl(INT64_C(1), 65) == 2);
    CHECK(jm::ushr(INT64_C(-1), 64) == -1);
    CHECK(jm::ushr(INT64_C(-1), 1) == kInt64Max);
}

TEST_CASE("ushr is a logical shift, shr is arithmetic", "[javamath][shift]") {
    CHECK(jm::shr(-8, 1) == -4);
    CHECK(jm::ushr(-8, 1) == 2147483644);
    CHECK(jm::shr(kInt32Min, 31) == -1);
    CHECK(jm::ushr(kInt32Min, 31) == 1);
}

// --- Properties that must hold for every input, not just the tabulated ones.

TEST_CASE("floorMod result carries the divisor's sign", "[javamath][property]") {
    constexpr auto kSamples = std::to_array<std::int32_t>(
        {kInt32Min, -1000003, -65536, -37, -1, 0, 1, 37, 65536, 1000003, kInt32Max});
    constexpr auto kDivisors = std::to_array<std::int32_t>({1, 2, 3, 16, 384, -1, -2, -3, -16});

    for (const std::int32_t x : kSamples) {
        for (const std::int32_t y : kDivisors) {
            CAPTURE(x, y);
            const std::int32_t mod = jm::floorMod(x, y);
            if (y > 0) {
                CHECK(mod >= 0);
                CHECK(mod < y);
            } else {
                CHECK(mod <= 0);
                CHECK(mod > y);
            }
            // The division identity holds under wrapping arithmetic.
            CHECK(jm::wrappingAdd(jm::wrappingMul(jm::floorDiv(x, y), y), mod) == x);
        }
    }
}

TEST_CASE("chunk-coordinate maths is continuous across zero", "[javamath][property]") {
    // The reason floorDiv exists here at all: `x / 16` would step twice at
    // the origin and mirror the world across x = 0, and `x % 16` would hand
    // back negative in-chunk offsets.
    std::int32_t previousChunk = jm::floorDiv(-64, 16);
    for (std::int32_t x = -63; x <= 64; ++x) {
        CAPTURE(x);
        const std::int32_t chunk = jm::floorDiv(x, 16);
        const std::int32_t offset = jm::floorMod(x, 16);
        CHECK(offset >= 0);
        CHECK(offset < 16);
        CHECK(jm::wrappingAdd(jm::wrappingMul(chunk, 16), offset) == x);
        CHECK((chunk - previousChunk) <= 1);
        CHECK(chunk >= previousChunk);
        previousChunk = chunk;
    }
}

TEST_CASE("wrapping arithmetic wraps at the Java boundaries", "[javamath][property]") {
    STATIC_REQUIRE(jm::wrappingAdd(kInt32Max, 1) == kInt32Min);
    STATIC_REQUIRE(jm::wrappingSub(kInt32Min, 1) == kInt32Max);
    STATIC_REQUIRE(jm::wrappingNegate(kInt32Min) == kInt32Min);
    STATIC_REQUIRE(jm::wrappingMul(kInt32Max, 2) == -2);
    STATIC_REQUIRE(jm::wrappingAdd(kInt64Max, INT64_C(1)) == kInt64Min);
    STATIC_REQUIRE(jm::wrappingNegate(kInt64Min) == kInt64Min);
    // The LCG multiplier, squared, as a wrapping 64-bit product.
    STATIC_REQUIRE(jm::wrappingMul(INT64_C(6364136223846793005), INT64_C(6364136223846793005)) ==
                   INT64_C(7520897724310334953));
}

TEST_CASE("long to int narrowing keeps the low 32 bits", "[javamath]") {
    STATIC_REQUIRE(jm::toInt(INT64_C(0)) == 0);
    STATIC_REQUIRE(jm::toInt(INT64_C(-1)) == -1);
    STATIC_REQUIRE(jm::toInt(INT64_C(4294967296)) == 0);
    STATIC_REQUIRE(jm::toInt(INT64_C(4294967297)) == 1);
    STATIC_REQUIRE(jm::toInt(kInt64Max) == -1);
    STATIC_REQUIRE(jm::toInt(kInt64Min) == 0);
    STATIC_REQUIRE(jm::toInt(INT64_C(0x1234'5678'9ABC'DEF0)) ==
                   static_cast<std::int32_t>(0x9ABCDEF0));
}

TEST_CASE("helpers are usable in constant expressions", "[javamath]") {
    // Pipeline compilation folds coordinate maths at compile time; if these
    // stopped being constexpr the loss would be silent.
    STATIC_REQUIRE(jm::floorDiv(-1, 16) == -1);
    STATIC_REQUIRE(jm::floorMod(-1, 16) == 15);
    STATIC_REQUIRE(jm::ushr(-1, 28) == 15);
    STATIC_REQUIRE(jm::shl(INT64_C(1), 40) == INT64_C(1099511627776));
}
