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

// ---------------------------------------------------------------------------
// The measured smear (SPEC §11). These pin what the sweep against vanilla
// established, as exact relationships rather than as statistics: the
// statistics live in tools/analysis and take minutes, but every one of them
// rests on the three claims below, and those are checkable in microseconds.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] BlendedNoise::Parameters overworldShape(double smearScaleMultiplier) {
    return BlendedNoise::Parameters{
        .xzScale = 0.25,
        .yScale = 0.125,
        .xzFactor = 80.0,
        .yFactor = 160.0,
        .smearScaleMultiplier = smearScaleMultiplier,
    };
}

[[nodiscard]] BlendedNoise built(bool measured, double smearScaleMultiplier) {
    JavaRandom random(0);
    return measured ? BlendedNoise::withModernReading(random, overworldShape(smearScaleMultiplier))
                    : BlendedNoise::legacy(random, overworldShape(smearScaleMultiplier));
}

} // namespace

TEST_CASE("at a multiplier of one the two readings agree above y = 0", "[noise][blended][smear]") {
    // The cap differs only in the slab it is built from and in what happens
    // below zero. At a multiplier of one the first difference vanishes, so
    // the whole of y >= 0 must come out bit-for-bit identical — which is what
    // keeps the cubiomes vectors, all 270 of them, meaningful for both.
    // Taking the divisors out is a division by exactly 128, which is exact in
    // binary, so this pins the normalisation ratio at the same time: if the
    // modern divisor were not exactly 128 times the old one, no height would
    // agree here at all.
    const BlendedNoise pre = built(false, 1.0);
    const BlendedNoise modern = built(true, 1.0);
    for (const int y : {0, 1, 2, 7, 64, 255, 319}) {
        for (const int x : {0, 13, -71}) {
            CAPTURE(x, y);
            CHECK(bits(pre.sample(x, y, 29) / 128.0) == bits(modern.sample(x, y, 29)));
        }
    }
}

TEST_CASE("below y = 0 the measured cap stops binding", "[noise][blended][smear]") {
    // Vanilla's spread below zero is flat in y, at the value its rising curve
    // above zero only reaches near y = 240. That is saturation: the fold runs
    // at full effect because nothing caps it. So the two readings must differ
    // here, at a multiplier of one, where above zero they cannot.
    const BlendedNoise pre = built(false, 1.0);
    const BlendedNoise modern = built(true, 1.0);
    std::size_t differing = 0;
    for (const int y : {-1, -8, -32, -63, -64}) {
        if (bits(pre.sample(11, y, 23) / 128.0) != bits(modern.sample(11, y, 23))) {
            ++differing;
        }
    }
    CHECK(differing == 5U);
}

TEST_CASE("the measured cap leaves the multiplier out of itself", "[noise][blended][smear]") {
    // The slab widens with the multiplier and the cap does not, so above y = 0
    // a multiplier of eight still differs from one — the fold is let in at a
    // different rate. Under PreModern both move together, which is exactly the
    // extrapolation that made the field scale with the multiplier.
    CHECK(bits(built(true, 1.0).sample(11, 96, 23)) != bits(built(true, 8.0).sample(11, 96, 23)));

    // The sharp one. PreModern's cap is built from the slab *with* the
    // multiplier and Measured's from the slab without it, so at any
    // multiplier but one they must part company above y = 0 too. If they
    // still agreed there, the cap would be taking the multiplier-bearing
    // slab and the whole distinction would be gone — which no statistic in
    // tools/analysis would notice quickly, and this notices at once.
    const BlendedNoise preEight = built(false, 8.0);
    const BlendedNoise modernEight = built(true, 8.0);
    // Not the top of the world: high enough up, the cap exceeds the local
    // offset under either reading, both saturate, and they agree again. That
    // is a real property and not a gap in the test — y = 255 and y = 319 come
    // out identical at a multiplier of eight.
    std::size_t parted = 0;
    for (const int y : {1, 7, 96, 128}) {
        if (bits(preEight.sample(11, y, 23) / 128.0) != bits(modernEight.sample(11, y, 23))) {
            ++parted;
        }
    }
    CHECK(parted == 4U);

    // And at y = 0 the cap is zero under either reading, so nothing folds and
    // the multiplier cannot matter. Measured, not assumed: it is why the
    // normalisation could be pinned at y = 0 without the smear confusing it.
    for (const double multiplier : {1.0, 4.0, 8.0, 16.0}) {
        CAPTURE(multiplier);
        CHECK(bits(built(true, multiplier).sample(11, 0, 23)) ==
              bits(built(true, 1.0).sample(11, 0, 23)));
    }
}
