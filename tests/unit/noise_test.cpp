// Stratum — noise tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Checked against cubiomes (MIT), the noise reference SPEC §2 names, through
// vectors in noise_vectors.inc. Doubles are compared as raw bits: these
// values feed straight into terrain shape, so a last-place difference is a
// different world.
//
// cubiomes is an independent reimplementation, so agreement is strong
// evidence rather than proof that either matches Mojang. That is settled by
// diffing generated terrain against the goldens (M3).

#include "noise_vectors.inc" // NOLINT(misc-include-cleaner) — generated

#include <stratum/noise/perlin.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using stratum::noise::NormalNoise;
using stratum::noise::OctaveNoise;
using stratum::noise::PerlinNoise;
using stratum::rng::Xoroshiro128PlusPlus;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] double fromBits(std::uint64_t value) noexcept {
    return std::bit_cast<double>(value);
}

[[nodiscard]] Xoroshiro128PlusPlus seeded(std::uint64_t seed) {
    return Xoroshiro128PlusPlus{static_cast<std::int64_t>(seed)};
}

} // namespace

TEST_CASE("the bounded draw matches cubiomes", "[rng][xoroshiro][noise][vectors]") {
    // The shuffle that builds every permutation table depends on this, so it
    // is checked before anything that uses it.
    for (const XNextIntVector& vector : kXNextIntVectors) {
        CAPTURE(vector.lo, vector.hi, vector.bound);
        Xoroshiro128PlusPlus random{stratum::rng::Seed128{vector.lo, vector.hi}};
        for (std::size_t i = 0; i < vector.values.size(); ++i) {
            CAPTURE(i);
            const std::int32_t value = random.nextInt(vector.bound);
            CHECK(value == vector.values[i]);
            CHECK(value >= 0);
            CHECK(value < vector.bound);
        }
    }
}

TEST_CASE("perlin initialisation matches cubiomes", "[noise][perlin][vectors]") {
    // Both halves of the state: the origin offsets and the full 256-entry
    // permutation. A shuffle that is right in aggregate but wrong in one
    // slot would still produce plausible-looking noise.
    for (const PerlinInitVector& vector : kPerlinInitVectors) {
        CAPTURE(vector.seed);
        Xoroshiro128PlusPlus random = seeded(vector.seed);
        const PerlinNoise noise = PerlinNoise::fromRandom(random);

        CHECK(bits(noise.originX()) == vector.originX);
        CHECK(bits(noise.originY()) == vector.originY);
        CHECK(bits(noise.originZ()) == vector.originZ);
        CHECK(noise.permutation() == vector.permutation);
    }
}

TEST_CASE("perlin sampling matches cubiomes, bit for bit", "[noise][perlin][vectors]") {
    for (const PerlinSampleVector& vector : kPerlinSampleVectors) {
        CAPTURE(vector.seed, vector.x, vector.y, vector.z);
        Xoroshiro128PlusPlus random = seeded(vector.seed);
        const PerlinNoise noise = PerlinNoise::fromRandom(random);
        CHECK(bits(noise.sample(fromBits(vector.x), fromBits(vector.y), fromBits(vector.z))) ==
              vector.value);
    }
}

TEST_CASE("simplex sampling matches cubiomes, bit for bit", "[noise][simplex][vectors]") {
    for (const SimplexSampleVector& vector : kSimplexSampleVectors) {
        CAPTURE(vector.seed, vector.x, vector.y);
        Xoroshiro128PlusPlus random = seeded(vector.seed);
        const PerlinNoise noise = PerlinNoise::fromRandom(random);
        CHECK(bits(noise.sampleSimplex2D(fromBits(vector.x), fromBits(vector.y))) == vector.value);
    }
}

TEST_CASE("octave and normal noise match cubiomes, bit for bit",
          "[noise][octave][normal][vectors]") {
    for (const OctaveSampleVector& vector : kOctaveSampleVectors) {
        CAPTURE(vector.seed, vector.firstOctave, vector.amplitudeCount);

        std::vector<double> amplitudes;
        amplitudes.reserve(static_cast<std::size_t>(vector.amplitudeCount));
        for (int i = 0; i < vector.amplitudeCount; ++i) {
            amplitudes.push_back(fromBits(vector.amplitudes[static_cast<std::size_t>(i)]));
        }

        const double x = fromBits(vector.x);
        const double y = fromBits(vector.y);
        const double z = fromBits(vector.z);

        Xoroshiro128PlusPlus octaveRandom = seeded(vector.seed);
        const OctaveNoise octave =
            OctaveNoise::create(octaveRandom, vector.firstOctave, amplitudes);
        CHECK(bits(octave.sample(x, y, z)) == vector.octaveValue);

        Xoroshiro128PlusPlus normalRandom = seeded(vector.seed);
        const NormalNoise normal =
            NormalNoise::create(normalRandom, vector.firstOctave, amplitudes);
        CHECK(bits(normal.sample(x, y, z)) == vector.doublePerlinValue);
    }
}

TEST_CASE("a zero amplitude skips its octave but not its frequency", "[noise][octave]") {
    // The distinction that makes a sparse amplitude list meaningful: the
    // octave contributes nothing, but the octaves after it keep their
    // frequencies.
    const std::vector<double> dense{1.0, 1.0, 1.0};
    const std::vector<double> sparse{1.0, 0.0, 1.0};

    Xoroshiro128PlusPlus denseRandom{INT64_C(42)};
    Xoroshiro128PlusPlus sparseRandom{INT64_C(42)};
    const OctaveNoise denseNoise = OctaveNoise::create(denseRandom, -3, dense);
    const OctaveNoise sparseNoise = OctaveNoise::create(sparseRandom, -3, sparse);

    CHECK(denseNoise.octaveCount() == 3U);
    CHECK(sparseNoise.octaveCount() == 2U);
    CHECK(bits(denseNoise.sample(1.5, 2.5, 3.5)) != bits(sparseNoise.sample(1.5, 2.5, 3.5)));
}

TEST_CASE("normal noise scales by the count of contributing octaves", "[noise][normal]") {
    // (5/3) * n / (n + 1), and leading and trailing zeroes do not count.
    const std::vector<double> two{1.0, 1.0};
    const std::vector<double> padded{0.0, 1.0, 1.0, 0.0};

    Xoroshiro128PlusPlus first{INT64_C(1)};
    Xoroshiro128PlusPlus second{INT64_C(1)};
    CHECK(bits(NormalNoise::create(first, -3, two).valueFactor()) ==
          bits(NormalNoise::create(second, -3, padded).valueFactor()));

    Xoroshiro128PlusPlus third{INT64_C(1)};
    CHECK(bits(NormalNoise::create(third, -3, two).valueFactor()) == bits(10.0 / 9.0));
}

TEST_CASE("the permutation is a permutation", "[noise][perlin]") {
    // A shuffle that dropped or duplicated an entry would still sample, and
    // would still look like noise.
    for (const std::int64_t seed : {INT64_C(0), INT64_C(1), INT64_C(-1), INT64_C(999)}) {
        CAPTURE(seed);
        Xoroshiro128PlusPlus random{seed};
        const PerlinNoise noise = PerlinNoise::fromRandom(random);

        std::array<bool, 256> seen{};
        for (const std::uint8_t entry : noise.permutation()) {
            CHECK_FALSE(seen[entry]);
            seen[entry] = true;
        }
    }
}
