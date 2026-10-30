// Stratum — a normal noise's two octave stacks, summed side by side.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// NormalNoise::sample sums its two stacks' octaves in one loop, which takes
// the stacks to be one length: every public construction builds them so,
// and the private constructor asserts it (perlin.cpp). This holds each
// construction to that, in a Debug build through the assert, and to the
// double the two stacks give summed one after the other — the octave noises
// drawn from the same generator in the same order, each OctaveNoise::sample
// added and scaled as NormalNoise spells it — over amplitude lists with zeros
// leading, trailing and inside, the legacy construction and the empty one.

#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using stratum::noise::NormalNoise;
using stratum::noise::OctaveNoise;
using stratum::rng::JavaRandom;
using stratum::rng::Xoroshiro128PlusPlus;

[[nodiscard]] std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

/// NormalNoise::sample's sum, written as two OctaveNoise sums.
[[nodiscard]] double separately(const OctaveNoise& first, const OctaveNoise& second,
                                double valueFactor, double x, double y, double z) {
    constexpr double kSecondFrequency = 337.0 / 331.0;
    const double combined =
        first.sample(x, y, z) +
        second.sample(x * kSecondFrequency, y * kSecondFrequency, z * kSecondFrequency);
    return combined * valueFactor;
}

/// Points either side of 0 and far out, fractional and whole.
[[nodiscard]] std::vector<std::array<double, 3>> points() {
    std::vector<std::array<double, 3>> out;
    Xoroshiro128PlusPlus random{std::int64_t{20261030}};
    for (int i = 0; i < 400; ++i) {
        const double scale = i % 4 == 0 ? 100000.0 : 300.0;
        out.push_back({(random.nextDouble() - 0.5) * scale, (random.nextDouble() - 0.5) * scale,
                       (random.nextDouble() - 0.5) * scale});
    }
    out.push_back({0.0, 0.0, 0.0});
    out.push_back({-0.0, -64.0, 255.0});
    return out;
}

} // namespace

TEST_CASE("a normal noise's two stacks are one length and sum as two octave noises do",
          "[noise][normal]") {
    const std::vector<std::vector<double>> amplitudeLists{
        {1.0},
        {1.0, 1.0},
        {0.0, 0.0, 1.0, 2.0, 0.0, 1.5},
        {1.0, 0.0, 0.0, 0.0, -1.0, 0.0},
        {0.0},
        {},
        {1.5, 0.25, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.5},
    };
    std::size_t compared = 0;
    for (std::size_t list = 0; list < amplitudeLists.size(); ++list) {
        const std::vector<double>& amplitudes = amplitudeLists[list];
        for (const int firstOctave : {-9, -4, 0}) {
            CAPTURE(list, firstOctave);
            const auto seed = static_cast<std::int64_t>((list * 131U) + 7U);
            Xoroshiro128PlusPlus drawn{seed};
            const NormalNoise noise = NormalNoise::create(drawn, firstOctave, amplitudes);
            Xoroshiro128PlusPlus again{seed};
            const OctaveNoise first = OctaveNoise::create(again, firstOctave, amplitudes);
            const OctaveNoise second = OctaveNoise::create(again, firstOctave, amplitudes);
            REQUIRE(first.octaveCount() == second.octaveCount());
            for (const std::array<double, 3>& at : points()) {
                CHECK(bits(noise.sample(at[0], at[1], at[2])) ==
                      bits(separately(first, second, noise.valueFactor(), at[0], at[1], at[2])));
                ++compared;
            }
        }
    }
    for (const int octaves : {1, 2, 8}) {
        CAPTURE(octaves);
        JavaRandom drawn{std::int64_t{octaves} * 99};
        const NormalNoise noise = NormalNoise::createLegacy(drawn, -octaves, octaves);
        JavaRandom again{std::int64_t{octaves} * 99};
        const OctaveNoise first = OctaveNoise::createLegacy(again, -octaves, octaves);
        const OctaveNoise second = OctaveNoise::createLegacy(again, -octaves, octaves);
        for (const std::array<double, 3>& at : points()) {
            CHECK(bits(noise.sample(at[0], at[1], at[2])) ==
                  bits(separately(first, second, noise.valueFactor(), at[0], at[1], at[2])));
            ++compared;
        }
    }
    const NormalNoise zero = NormalNoise::zero();
    CHECK(bits(zero.sample(3.5, -2.0, 17.25)) == bits(0.0));
    CHECK(compared > 8000U);
}
