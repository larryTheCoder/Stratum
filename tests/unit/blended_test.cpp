// Stratum — the legacy blended noise, against cubiomes.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The vectors here pin three things: the LCG-seeded Perlin construction, the
// order the three octave stacks are drawn in, and the octave loop and blend
// that turn them into a value.
//
// They pin nothing about `smear_scale_multiplier`, because cubiomes models
// the pre-1.18 noise and that parameter did not exist then; nothing about
// the coordinate wrap, because cubiomes has it commented out; and nothing
// about how a dimension that does not declare `legacy_random_source` seeds
// any of it. Those three gaps are why `old_blended_noise` is still refused
// by the interpreter, and this file is careful not to look like it closed
// them.

#include "blended_vectors.inc"

#include <stratum/noise/blended.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>

namespace {

using stratum::noise::BlendedNoise;
using stratum::noise::PerlinNoise;
using stratum::rng::JavaRandom;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] double fromBits(std::uint64_t value) noexcept {
    return std::bit_cast<double>(value);
}

} // namespace

TEST_CASE("the LCG-seeded Perlin construction matches cubiomes", "[noise][blended]") {
    std::int64_t currentSeed = kLegacyPerlinVectors.front().seed;
    JavaRandom random(currentSeed);
    int drawn = 0;

    for (const auto& vector : kLegacyPerlinVectors) {
        if (vector.seed != currentSeed) {
            currentSeed = vector.seed;
            random.setSeed(currentSeed);
            drawn = 0;
        }
        // The vectors skip most indices, so the generator is advanced to the
        // one being checked. That is the point of including index 39: an
        // implementation that reseeded between octaves would agree on the
        // first three and diverge here.
        while (drawn < vector.index) {
            static_cast<void>(PerlinNoise::fromRandom(random));
            ++drawn;
        }
        const PerlinNoise noise = PerlinNoise::fromRandom(random);
        ++drawn;

        CAPTURE(vector.seed, vector.index);
        CHECK(bits(noise.originX()) == vector.originX);
        CHECK(bits(noise.originY()) == vector.originY);
        CHECK(bits(noise.originZ()) == vector.originZ);
        CHECK(noise.permutation() == vector.permutation);
    }
}

TEST_CASE("the blended noise matches cubiomes octave for octave", "[noise][blended]") {
    for (const auto& vector : kBlendedVectors) {
        // A multiplier of one is the pre-1.18 behaviour, which is the only
        // one cubiomes can speak to.
        const BlendedNoise::Parameters parameters{.xzScale = fromBits(vector.xzScale),
                                                  .yScale = fromBits(vector.yScale),
                                                  .xzFactor = fromBits(vector.xzFactor),
                                                  .yFactor = fromBits(vector.yFactor),
                                                  .smearScaleMultiplier = 1.0};
        JavaRandom random(vector.seed);
        const BlendedNoise noise = BlendedNoise::legacy(random, parameters);

        CAPTURE(vector.seed, vector.x, vector.y, vector.z);
        CHECK(bits(noise.sample(vector.x, vector.y, vector.z)) == vector.value);
    }
}

TEST_CASE("the coordinate wrap folds into a band around the origin", "[noise][blended]") {
    // Not checked against cubiomes, which leaves this out: the constant and
    // the rounding are the documented ones and the behaviour is asserted
    // here so that it is at least written down and cannot drift silently.
    const auto wrap = [](double value) { return stratum::noise::maintainPrecision(value); };

    // Identity everywhere a world actually reaches. This is why cubiomes can
    // omit it and still agree with vanilla in practice.
    CHECK(bits(wrap(0.0)) == bits(0.0));
    CHECK(bits(wrap(1000.0)) == bits(1000.0));
    CHECK(bits(wrap(-1000.0)) == bits(-1000.0));
    CHECK(bits(wrap(16777215.0)) == bits(16777215.0));

    // Past half the period it folds by a whole period, and the result stays
    // in the same band rather than growing without bound.
    constexpr double kPeriod = 33554432.0;
    CHECK(bits(wrap(kPeriod)) == bits(0.0));
    CHECK(bits(wrap(kPeriod + 5.0)) == bits(5.0));
    CHECK(bits(wrap(-kPeriod - 5.0)) == bits(-5.0));
    CHECK(wrap(1.0e12) <= kPeriod / 2.0);
    CHECK(wrap(1.0e12) >= -kPeriod / 2.0);

    // Java rounds half up, so the fold at exactly one half goes upward — the
    // one place C's round() would disagree.
    CHECK(bits(wrap(kPeriod / 2.0)) == bits(-kPeriod / 2.0));
}

TEST_CASE("the smear multiplier is part of the answer", "[noise][blended]") {
    const BlendedNoise::Parameters base{.xzScale = 0.25,
                                        .yScale = 0.125,
                                        .xzFactor = 80.0,
                                        .yFactor = 160.0,
                                        .smearScaleMultiplier = 1.0};
    BlendedNoise::Parameters smeared = base;
    smeared.smearScaleMultiplier = 8.0;

    JavaRandom first(42);
    const BlendedNoise plain = BlendedNoise::legacy(first, base);
    JavaRandom second(42);
    const BlendedNoise wide = BlendedNoise::legacy(second, smeared);

    // The fold is `d2 -= floor(min(y * smear, d2) / smear) * smear` over a
    // d2 in [0, 1). At y = 0 the clamp is zero whatever the smear is, so the
    // multiplier cannot show up at all — but that is the only place it
    // cannot. At any other height the later octaves have a smear small
    // enough for the clamp to pick `y * smear`, and the fold is then a whole
    // number of slabs that the multiplier decides the width of.
    CHECK(bits(plain.sample(0, 0, 0)) == bits(wide.sample(0, 0, 0)));

    // Everywhere else it is the difference between two worlds — above the
    // ground as much as below it.
    CHECK(bits(plain.sample(17, 33, -49)) != bits(wide.sample(17, 33, -49)));
    CHECK(bits(plain.sample(17, -1, -49)) != bits(wide.sample(17, -1, -49)));
    CHECK(bits(plain.sample(0, -60, 0)) != bits(wide.sample(0, -60, 0)));
}

TEST_CASE("the three stacks are drawn from one generator, in order", "[noise][blended]") {
    const BlendedNoise::Parameters parameters{.xzScale = 0.25,
                                              .yScale = 0.125,
                                              .xzFactor = 80.0,
                                              .yFactor = 160.0,
                                              .smearScaleMultiplier = 1.0};

    JavaRandom random(7);
    const BlendedNoise noise = BlendedNoise::legacy(random, parameters);

    // Forty octaves: sixteen, sixteen and eight. A construction that reset
    // the generator per stack, or drew them in another order, would leave it
    // somewhere else entirely.
    JavaRandom counted(7);
    for (int i = 0; i < 40; ++i) {
        static_cast<void>(PerlinNoise::fromRandom(counted));
    }
    CHECK(random.nextLong() == counted.nextLong());

    // And the noise is not accidentally flat.
    CHECK(bits(noise.sample(1, 2, 3)) != bits(noise.sample(4, 5, 6)));
}
