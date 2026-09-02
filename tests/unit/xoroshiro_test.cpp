// Stratum — Xoroshiro128++ tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Each suite below says which oracle backs it, because they are not equally
// strong: the generator and the mix are checked against the JDK's own
// implementations, while the seed upgrade is checked against an independent
// implementation of the documented formula. See the header for what that
// distinction means.

#include "xoroshiro_vectors.inc" // NOLINT(misc-include-cleaner) — generated

#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>

namespace {

using stratum::rng::Seed128;
using stratum::rng::Xoroshiro128PlusPlus;

[[nodiscard]] std::uint64_t doubleBits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint32_t floatBits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

} // namespace

TEST_CASE("the generator matches the JDK's Xoroshiro128PlusPlus", "[rng][xoroshiro][vectors]") {
    // Strongest oracle available: the JDK's implementation seeded to exactly
    // the same 128-bit state.
    for (const XoroshiroSequence& vector : kNextLongVectors) {
        CAPTURE(vector.lo, vector.hi);
        Xoroshiro128PlusPlus generator{Seed128{vector.lo, vector.hi}};
        for (std::size_t i = 0; i < vector.values.size(); ++i) {
            CAPTURE(i);
            CHECK(generator.nextLong() == vector.values[i]);
        }
    }
}

TEST_CASE("mixStafford13 matches the JDK's", "[rng][xoroshiro][vectors]") {
    for (const MixVector& vector : kMixStafford13Vectors) {
        CAPTURE(vector.input);
        CHECK(stratum::rng::mixStafford13(vector.input) == vector.mixed);
    }
}

TEST_CASE("the seed upgrade matches an independent implementation of the documented formula",
          "[rng][xoroshiro][vectors]") {
    // Weaker than the two above, and deliberately labelled so: this proves
    // our port is faithful, not that Mojang composes it this way. That is
    // settled by diffing generated terrain against the goldens (M3).
    for (const SeedUpgradeVector& vector : kSeedUpgradeVectors) {
        CAPTURE(vector.seed);
        const Seed128 upgraded = stratum::rng::upgradeSeedTo128Bit(vector.seed);
        CHECK(upgraded.lo == vector.lo);
        CHECK(upgraded.hi == vector.hi);
        // The upgrade must never hand the generator a state it cannot leave.
        CHECK_FALSE(upgraded.degenerate());
    }
}

TEST_CASE("the derived draws match the JDK's", "[rng][xoroshiro][vectors]") {
    for (const DerivedVector& vector : kDerivedVectors) {
        CAPTURE(vector.lo, vector.hi);
        const Seed128 state{vector.lo, vector.hi};

        Xoroshiro128PlusPlus ints{state};
        for (std::size_t i = 0; i < vector.nextInt.size(); ++i) {
            CAPTURE(i);
            CHECK(ints.nextInt() == vector.nextInt[i]);
        }

        Xoroshiro128PlusPlus doubles{state};
        for (std::size_t i = 0; i < vector.nextDoubleBits.size(); ++i) {
            CAPTURE(i);
            CHECK(doubleBits(doubles.nextDouble()) == vector.nextDoubleBits[i]);
        }

        Xoroshiro128PlusPlus floats{state};
        for (std::size_t i = 0; i < vector.nextFloatBits.size(); ++i) {
            CAPTURE(i);
            CHECK(floatBits(floats.nextFloat()) == vector.nextFloatBits[i]);
        }

        Xoroshiro128PlusPlus booleans{state};
        for (std::size_t i = 0; i < vector.nextBoolean.size(); ++i) {
            CAPTURE(i);
            CHECK(booleans.nextBoolean() == vector.nextBoolean[i]);
        }
    }
}

TEST_CASE("next(bits) takes the high bits of the draw", "[rng][xoroshiro]") {
    const Seed128 state{1U, 2U};
    Xoroshiro128PlusPlus reference{state};
    const auto draw = static_cast<std::uint64_t>(reference.nextLong());

    for (const int bits : {1, 4, 16, 31, 32}) {
        CAPTURE(bits);
        Xoroshiro128PlusPlus generator{state};
        const auto expected = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(draw >> static_cast<unsigned>(64 - bits)));
        CHECK(generator.next(bits) == expected);
    }
}

TEST_CASE("distinct seeds give distinct states", "[rng][xoroshiro]") {
    // A seed upgrade that collapsed nearby seeds together would make whole
    // worlds identical, which no single vector would reveal.
    const Seed128 zero = stratum::rng::upgradeSeedTo128Bit(0);
    const Seed128 one = stratum::rng::upgradeSeedTo128Bit(1);
    const Seed128 minusOne = stratum::rng::upgradeSeedTo128Bit(-1);

    CHECK_FALSE(zero == one);
    CHECK_FALSE(zero == minusOne);
    CHECK_FALSE(one == minusOne);
    CHECK(zero.lo != zero.hi);
}

TEST_CASE("copying a generator forks the stream", "[rng][xoroshiro]") {
    Xoroshiro128PlusPlus original{INT64_C(12345)};
    (void)original.nextLong();

    Xoroshiro128PlusPlus fork = original;
    for (int i = 0; i < 16; ++i) {
        CAPTURE(i);
        CHECK(fork.nextLong() == original.nextLong());
    }
}

TEST_CASE("the generator is usable in constant expressions", "[rng][xoroshiro]") {
    STATIC_REQUIRE(stratum::rng::mixStafford13(0U) == 0U);
    STATIC_REQUIRE([] {
        Xoroshiro128PlusPlus generator{Seed128{1U, 0U}};
        return generator.nextLong();
    }() == 131073);
}
