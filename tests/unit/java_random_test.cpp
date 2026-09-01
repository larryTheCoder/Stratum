// Stratum — java.util.Random parity tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Every expectation here is observed JVM output (tools/vectors/
// JavaRandomVectors.java). Floating point is compared as raw bits: SPEC §7
// Tier A is bit-exact, so an "almost equal" double is a failure.
//
// nextGaussian is absent from both the generator and these tests: it needs
// StrictMath.log (fdlibm), and glibc's log differs by an ulp. See SPEC §11.

#include "java_random_vectors.inc" // NOLINT(misc-include-cleaner) — generated

#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>

namespace {

using stratum::rng::JavaRandom;

[[nodiscard]] std::uint64_t doubleBits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint32_t floatBits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

} // namespace

TEST_CASE("nextInt matches the JVM", "[rng][javarandom][vectors]") {
    for (const IntSequence& vector : kNextIntVectors) {
        CAPTURE(vector.seed);
        JavaRandom random(vector.seed);
        for (std::size_t i = 0; i < vector.values.size(); ++i) {
            CAPTURE(i);
            CHECK(random.nextInt() == vector.values[i]);
        }
    }
}

TEST_CASE("nextInt(bound) matches the JVM on both the power-of-two fast path "
          "and the rejection loop",
          "[rng][javarandom][vectors]") {
    for (const BoundedIntSequence& vector : kNextIntBoundedVectors) {
        CAPTURE(vector.seed, vector.bound);
        JavaRandom random(vector.seed);
        for (std::size_t i = 0; i < vector.values.size(); ++i) {
            CAPTURE(i);
            const std::int32_t value = random.nextInt(vector.bound);
            CHECK(value == vector.values[i]);
            CHECK(value >= 0);
            CHECK(value < vector.bound);
        }
    }
}

TEST_CASE("nextLong matches the JVM", "[rng][javarandom][vectors]") {
    for (const LongSequence& vector : kNextLongVectors) {
        CAPTURE(vector.seed);
        JavaRandom random(vector.seed);
        for (std::size_t i = 0; i < vector.values.size(); ++i) {
            CAPTURE(i);
            CHECK(random.nextLong() == vector.values[i]);
        }
    }
}

TEST_CASE("nextBoolean matches the JVM", "[rng][javarandom][vectors]") {
    for (const BoolSequence& vector : kNextBooleanVectors) {
        CAPTURE(vector.seed);
        JavaRandom random(vector.seed);
        for (std::size_t i = 0; i < vector.values.size(); ++i) {
            CAPTURE(i);
            CHECK(random.nextBoolean() == vector.values[i]);
        }
    }
}

TEST_CASE("nextFloat is bit-exact against the JVM", "[rng][javarandom][vectors]") {
    for (const FloatBitsSequence& vector : kNextFloatVectors) {
        CAPTURE(vector.seed);
        JavaRandom random(vector.seed);
        for (std::size_t i = 0; i < vector.bits.size(); ++i) {
            CAPTURE(i);
            CHECK(floatBits(random.nextFloat()) == vector.bits[i]);
        }
    }
}

TEST_CASE("nextDouble is bit-exact against the JVM", "[rng][javarandom][vectors]") {
    for (const DoubleBitsSequence& vector : kNextDoubleVectors) {
        CAPTURE(vector.seed);
        JavaRandom random(vector.seed);
        for (std::size_t i = 0; i < vector.bits.size(); ++i) {
            CAPTURE(i);
            CHECK(doubleBits(random.nextDouble()) == vector.bits[i]);
        }
    }
}

TEST_CASE("mixed call orders stay in step with the JVM", "[rng][javarandom][vectors]") {
    // Each method consumes a different number of LCG steps. Getting one
    // wrong desynchronises everything after it while every value in
    // isolation still looks plausible, which is exactly how a parity bug
    // hides.
    for (const InterleavedSequence& vector : kInterleavedVectors) {
        CAPTURE(vector.seed);
        JavaRandom random(vector.seed);
        CHECK(random.nextInt() == vector.values[0]);
        CHECK(static_cast<std::int64_t>(random.nextBoolean() ? 1 : 0) == vector.values[1]);
        CHECK(random.nextLong() == vector.values[2]);
        CHECK(random.nextInt(17) == vector.values[3]);
        CHECK(static_cast<std::int64_t>(doubleBits(random.nextDouble())) == vector.values[4]);
        CHECK(static_cast<std::int64_t>(floatBits(random.nextFloat())) == vector.values[5]);
        CHECK(random.nextInt(1 << 30) == vector.values[6]);
        CHECK(random.nextInt() == vector.values[7]);
        CHECK(random.nextInt(3) == vector.values[8]);
        CHECK(static_cast<std::int64_t>(floatBits(random.nextFloat())) == vector.values[9]);
        CHECK(random.nextLong() == vector.values[10]);
        CHECK(random.nextInt(100) == vector.values[11]);
    }
}

TEST_CASE("seed scrambling matches the specified initial state", "[rng][javarandom]") {
    // seed ^ multiplier, masked to 48 bits.
    STATIC_REQUIRE(JavaRandom(0).state() == JavaRandom::kMultiplier);
    STATIC_REQUIRE(JavaRandom(static_cast<std::int64_t>(JavaRandom::kMultiplier)).state() == 0U);
    STATIC_REQUIRE(JavaRandom(-1).state() == (~JavaRandom::kMultiplier & JavaRandom::kMask));
    STATIC_REQUIRE(JavaRandom(0).state() <= JavaRandom::kMask);
}

TEST_CASE("setSeed restarts the stream", "[rng][javarandom]") {
    JavaRandom random(12345);
    const std::int32_t first = random.nextInt();
    const std::uint64_t second = doubleBits(random.nextDouble());

    random.setSeed(12345);
    CHECK(random.nextInt() == first);
    CHECK(doubleBits(random.nextDouble()) == second);

    // Reseeding to a different value must actually change the stream.
    random.setSeed(12346);
    CHECK(random.nextInt() != first);
}

TEST_CASE("copying a generator forks the stream", "[rng][javarandom]") {
    // Worldgen derives generators per (seed, position, salt) and never
    // shares one; copies must be independent, not aliases.
    JavaRandom original(987654321);
    (void)original.nextInt();

    JavaRandom fork = original;
    for (int i = 0; i < 16; ++i) {
        CAPTURE(i);
        CHECK(fork.nextInt() == original.nextInt());
    }
}

TEST_CASE("the generator is usable in constant expressions", "[rng][javarandom]") {
    STATIC_REQUIRE([] {
        JavaRandom random(0);
        return random.nextInt();
    }() == -1155484576);
    STATIC_REQUIRE([] {
        JavaRandom random(0);
        return random.nextLong();
    }() == INT64_C(-4962768465676381896));
}
