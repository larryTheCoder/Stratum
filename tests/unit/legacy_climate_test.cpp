// Stratum — the legacy climate noises, against cubiomes.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The vectors pin the SHIPPED registry's legacy climate rule against
// cubiomes' `setNetherSeed`, bit for bit: temperature from
// java.util.Random(worldSeed), vegetation from (worldSeed + 1), two octaves at
// first octave -7 drawn highest frequency first, and an `offset` that is zero.
// That the rule is vanilla's, and not only cubiomes', is measured separately
// against the golden Nether (vanilla_legacy_nether_climate_gap_test.cpp,
// SPEC §11); these cases are what make a regression in the construction
// itself fail without fixtures.

#include "nether_climate_vectors.inc"

#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::density::NoiseParameters;
using stratum::density::NoiseRegistry;
using stratum::density::RandomSource;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::vector<ResourceLocation> climateNames() {
    return {ResourceLocation::parse("minecraft:temperature"),
            ResourceLocation::parse("minecraft:vegetation"),
            ResourceLocation::parse("minecraft:offset")};
}

} // namespace

TEST_CASE("the legacy registry builds cubiomes' Nether climate, bit for bit",
          "[noise][legacy][climate]") {
    const std::map<ResourceLocation, NoiseParameters> none;
    const auto names = climateNames();
    for (const NetherClimateVector& vector : kNetherClimateVectors) {
        CAPTURE(vector.seed, vector.x, vector.z);
        const NoiseRegistry registry =
            NoiseRegistry::create(none, names, vector.seed, RandomSource::Legacy);
        const auto x = static_cast<double>(vector.x);
        const auto z = static_cast<double>(vector.z);
        CHECK(bits(registry.get(names[0]).sample(x, 0.0, z)) == vector.temperature);
        CHECK(bits(registry.get(names[1]).sample(x, 0.0, z)) == vector.humidity);
        // No shift: whatever the position, `offset` contributes nothing.
        CHECK(bits(registry.get(names[2]).sample(x, 0.0, z)) == bits(0.0));
        CHECK(bits(registry.get(names[2]).sample(z * 0.25, x, -17.5)) == bits(0.0));
    }
}

TEST_CASE("the legacy NormalNoise is the construction the vectors pin",
          "[noise][legacy][climate]") {
    // One layer down from the registry, so that a failure there can be told
    // apart from one in how the registry seeds it. Temperature's generator is
    // the world seed handed straight to the LCG.
    for (const NetherClimateVector& vector : kNetherClimateVectors) {
        CAPTURE(vector.seed, vector.x, vector.z);
        stratum::rng::JavaRandom random{vector.seed};
        const auto noise = stratum::noise::NormalNoise::createLegacy(random, -7, 2);
        CHECK(bits(noise.sample(static_cast<double>(vector.x), 0.0,
                                static_cast<double>(vector.z))) == vector.temperature);
    }
}

TEST_CASE("the legacy climate ignores the pack's parameters for its three noises",
          "[noise][legacy][climate]") {
    // The documented reading, carried rather than measured (SPEC §11):
    // vanilla's own shipped parameters for these names are not what its
    // legacy Nether uses, so neither is anything a pack writes there. Pinned
    // so that changing it is a decision with a failing test, not a drift.
    const auto names = climateNames();
    std::map<ResourceLocation, NoiseParameters> unusual;
    for (const ResourceLocation& id : names) {
        unusual.emplace(id, NoiseParameters{.firstOctave = -3, .amplitudes = {1.5, 0.0, 2.0}});
    }
    const NoiseRegistry withParameters =
        NoiseRegistry::create(unusual, names, 42, RandomSource::Legacy);
    const std::map<ResourceLocation, NoiseParameters> none;
    const NoiseRegistry without = NoiseRegistry::create(none, names, 42, RandomSource::Legacy);
    for (const ResourceLocation& id : names) {
        CAPTURE(id.toString());
        for (const double at : {-300.0, 0.0, 31.0, 4096.0}) {
            CHECK(bits(withParameters.get(id).sample(at, 0.0, -at)) ==
                  bits(without.get(id).sample(at, 0.0, -at)));
        }
    }
}

TEST_CASE("any other legacy noise is still refused, and named alone", "[noise][legacy][climate]") {
    auto names = climateNames();
    names.push_back(ResourceLocation::parse("minecraft:surface"));
    names.push_back(ResourceLocation::parse("minecraft:surface"));
    const std::map<ResourceLocation, NoiseParameters> none;

    // The refusal names what is still unsolved, once, and none of the three
    // it can build: listing those would send a reader after a solved problem.
    try {
        static_cast<void>(NoiseRegistry::create(none, names, 0, RandomSource::Legacy));
        FAIL("a legacy registry naming minecraft:surface was built");
    } catch (const stratum::density::NoiseError& error) {
        const std::string message = error.what();
        CHECK_THAT(message, ContainsSubstring("legacy_random_source") &&
                                ContainsSubstring("names 1 noise(s) — minecraft:surface —") &&
                                ContainsSubstring("will not substitute"));
        CHECK_THAT(message, !ContainsSubstring("names 3") && !ContainsSubstring("names 4"));
    }

    // And the modern source is untouched by any of this: it still reads the
    // pack's parameters, so it still needs them.
    CHECK_THROWS_WITH(NoiseRegistry::create(none, climateNames(), 0, RandomSource::Xoroshiro),
                      ContainsSubstring("no parameters for noise"));
}

TEST_CASE("the legacy octave construction refuses a shape octaveInit does not define",
          "[noise][legacy][climate]") {
    stratum::rng::JavaRandom random{0};
    CHECK_THROWS_AS(stratum::noise::OctaveNoise::createLegacy(random, -7, 0),
                    std::invalid_argument);
    // Top octave 1: above zero, where cubiomes refuses too.
    CHECK_THROWS_AS(stratum::noise::OctaveNoise::createLegacy(random, 0, 2), std::invalid_argument);
    // Top octave exactly 0 is defined, with nothing skipped.
    CHECK(stratum::noise::OctaveNoise::createLegacy(random, -1, 2).octaveCount() == 2U);
}
