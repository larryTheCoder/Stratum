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

TEST_CASE("the legacy construction is not what a modern dimension uses",
          "[noise][blended][oracle]") {
    // stratum::noise::BlendedNoise matches cubiomes bit-exactly, and this is
    // the demonstration that bit-exactness against cubiomes was never the
    // question: cubiomes answers about the pre-1.18 noise, seeded from a Java
    // LCG and living in a ±128 density space.
    //
    // Twenty-four candidate derivations of the modern seeding — sequential
    // and per-stack draws, six salt strings, amplitudes doubling and halving
    // — produced no constant ratio against these vectors either. The search
    // space is larger than guessing covers, which is why the node stays
    // refused (SPEC §11).
    std::size_t matches = 0;
    for (const auto& vector : kDeepslateBlendedVectors) {
        const BlendedNoise::Parameters parameters{.xzScale = fromBits(vector.xzScale),
                                                  .yScale = fromBits(vector.yScale),
                                                  .xzFactor = fromBits(vector.xzFactor),
                                                  .yFactor = fromBits(vector.yFactor),
                                                  .smearScaleMultiplier =
                                                      fromBits(vector.smearScaleMultiplier)};
        JavaRandom random(vector.seed);
        const BlendedNoise legacy = BlendedNoise::legacy(random, parameters);
        if (std::bit_cast<std::uint64_t>(legacy.sample(vector.x, vector.y, vector.z)) ==
            vector.value) {
            ++matches;
        }
    }

    // When this stops being zero, old_blended_noise has been solved and this
    // case should become the real comparison.
    CHECK(matches == 0U);
}
