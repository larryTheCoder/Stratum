// Stratum — what the deepslate vectors establish about old_blended_noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// These vectors are an oracle for a function this build cannot yet compute,
// so this file cannot check an implementation against them. What it does
// instead is pin what they establish about the problem, so the file is not
// inert while the problem is open, and so the findings behind SPEC §11's
// three unsettled questions are held by a test rather than by prose.
//
// The day someone implements old_blended_noise, the last case here starts
// failing and should be replaced by the real comparison — which is the point
// of writing it this way round.

#include "deepslate_vectors.inc"

#include <stratum/noise/blended.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>

namespace {

using stratum::noise::BlendedNoise;
using stratum::rng::JavaRandom;

[[nodiscard]] double fromBits(std::uint64_t value) noexcept {
    return std::bit_cast<double>(value);
}

} // namespace

TEST_CASE("the deepslate vectors cover what they need to", "[noise][blended][oracle]") {
    CHECK(kDeepslateBlendedVectors.size() == 240U);

    std::set<std::int64_t> seeds;
    std::set<std::string_view> shapes;
    bool anyNegativeY = false;
    for (const auto& vector : kDeepslateBlendedVectors) {
        seeds.insert(vector.seed);
        shapes.insert(vector.shape);
        anyNegativeY = anyNegativeY || vector.y < 0;
    }

    CHECK(seeds.size() == 5U);
    // The three vanilla dimensions use, plus one that is nobody's — an
    // implementation that special-cased the known shapes would still have to
    // be right about the fourth.
    CHECK(shapes.size() == 4U);
    // The smear folds nothing at y = 0 and something everywhere else, so a
    // vector set that stayed above ground would leave the multiplier
    // untested.
    CHECK(anyNegativeY);
}

TEST_CASE("old_blended_noise lands in a density space of order one", "[noise][blended][oracle]") {
    // This is the finding that corrected SPEC §11, now held against a real
    // oracle rather than against an argument. `sloped_cheese` is
    // `4 * quarter_negative(...) + base_3d_noise`, whose other term is order
    // one, and these values agree.
    double largest = 0.0;
    for (const auto& vector : kDeepslateBlendedVectors) {
        const double value = fromBits(vector.value);
        REQUIRE(std::isfinite(value));
        largest = std::max(largest, std::abs(value));
    }
    CHECK(largest < 4.0);
    CHECK(largest > 0.1);
}

TEST_CASE("the modern construction reproduces vanilla's own values", "[noise][blended][oracle]") {
    // This case used to assert that nothing matched, with a note saying it
    // should become the real comparison once the node was solved. It has.
    //
    // Seeding: one generator from the world seed's positional factory under
    // the name `minecraft:terrain`, then sixteen minimum, sixteen maximum and
    // eight blend octaves drawn from it in order. Reading: the octave stack
    // over 512 and then 128, the smear cap built from the pre-multiplier slab
    // and not binding below y = 0, and the arithmetic in the order
    // spec/blended-noise-spec.md's floating-point amendments state. Every one
    // of those was established separately (SPEC §11); this is all of them at
    // once against numbers neither of them was fitted to.
    std::size_t exact = 0;
    double worst = 0.0;
    for (const auto& vector : kDeepslateBlendedVectors) {
        const BlendedNoise::Parameters parameters{.xzScale = fromBits(vector.xzScale),
                                                  .yScale = fromBits(vector.yScale),
                                                  .xzFactor = fromBits(vector.xzFactor),
                                                  .yFactor = fromBits(vector.yFactor),
                                                  .smearScaleMultiplier =
                                                      fromBits(vector.smearScaleMultiplier)};
        const BlendedNoise modern = BlendedNoise::modern(vector.seed, parameters);
        const double got = modern.sample(vector.x, vector.y, vector.z);
        if (std::bit_cast<std::uint64_t>(got) == vector.value) {
            ++exact;
        }
        worst = std::max(worst, std::abs(got - fromBits(vector.value)));
    }

    // Every one of the 240, bit for bit. Until the floating-point form was
    // settled (SPEC §11, "old_blended_noise's floating-point form") ten were
    // not: off by at most 6.3e-14, five of them at (1000, -60, -1000), which
    // this case put down to accumulated rounding. It was the order the
    // position and the selector are formed in — cubiomes' x * (M / F) and
    // 0.5 + 0.05 * main against the engine's (x * M) / F and (main / 10 + 1) / 2
    // — which the server, not deepslate, settled
    // (tests/conformance/vanilla_blended_fpform_test.cpp).
    CHECK_FALSE(worst > 0.0);
    CHECK(exact == 240U);

    // The salt is load-bearing, not decoration: it is MD5'd into the seed, so
    // a near-miss name gives an unrelated world rather than a nearby one.
    // Without this, "the seeding is right" would rest on one positive result.
    const BlendedNoise::Parameters overworld{.xzScale = 0.25,
                                             .yScale = 0.125,
                                             .xzFactor = 80.0,
                                             .yFactor = 160.0,
                                             .smearScaleMultiplier = 8.0};
    const BlendedNoise right = BlendedNoise::modern(0, overworld);
    JavaRandom legacySeeded(0);
    const BlendedNoise wrong = BlendedNoise::withModernReading(legacySeeded, overworld);
    CHECK(std::abs(right.sample(17, 33, -49) - wrong.sample(17, 33, -49)) > 1.0e-3);
}
