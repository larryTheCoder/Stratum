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
    // over 65536, and the smear cap built from the pre-multiplier slab and
    // not binding below y = 0. Every one of those was established separately
    // (SPEC §11); this is all of them at once against numbers neither of them
    // was fitted to.
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

    // The claim. Every one of the 240, to within a part in a billion.
    CHECK(worst < 1.0e-9);

    // And 230 of them bit-for-bit. The ten that are not differ by at most
    // 6.3e-14 and sit at the largest coordinates in the set — five of them at
    // (1000, -60, -1000) — which is accumulated rounding across sixteen
    // octaves whose amplitudes reach 32768, not a difference of construction.
    // Whether the last bits are ours or deepslate's is not something deepslate
    // can settle: it is an emulator, and the golden regions are the authority
    // (SPEC §7). Pinned as a number so that a platform disagreeing here has to
    // be looked at rather than absorbed.
    CHECK(exact == 230U);

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
