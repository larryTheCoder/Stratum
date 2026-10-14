// Stratum — reading the biome parameter table and searching it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The table itself is Mojang-derived and never committed, so what is checked
// here is the reading and the search over tables written by this file. The
// real one is checked in the conformance suite, against the biomes vanilla
// itself recorded.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::biome::ClimateSample;
using stratum::biome::Parameter;
using stratum::biome::ParameterError;
using stratum::biome::ParameterList;
using stratum::biome::ParameterPoint;
using stratum::biome::quantizeCoord;
using stratum::biome::QuantizedPoint;
using stratum::biome::QuantizedSample;
using stratum::data::ResourceLocation;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] nlohmann::json entry(const std::string& biome, double temperature,
                                   double continentalness, double offset = 0.0) {
    return nlohmann::json{
        {"biome", biome},
        {"parameters",
         {{"temperature", nlohmann::json::array({temperature, temperature})},
          {"humidity", nlohmann::json::array({-1.0, 1.0})},
          {"continentalness", nlohmann::json::array({continentalness, continentalness})},
          {"erosion", nlohmann::json::array({-1.0, 1.0})},
          {"depth", 0.0},
          {"weirdness", nlohmann::json::array({-1.0, 1.0})},
          {"offset", offset}}}};
}

[[nodiscard]] ParameterList parse(const nlohmann::json& json) {
    return ParameterList::fromJson(json, ResourceLocation::parse("minecraft:overworld"));
}

} // namespace

TEST_CASE("a range is read from either spelling", "[biome][parameters]") {
    // The server writes `depth: 0.0` beside `continentalness: [-1.2, -1.05]`,
    // so a bare number is a range of zero width and not a malformed pair.
    const ParameterList list =
        parse(nlohmann::json{{"biomes", {entry("minecraft:plains", 0.5, 0.25)}}});
    REQUIRE(list.size() == 1U);
    const auto& parameters = list.entries()[0].parameters;
    CHECK(bits(parameters.depth.min) == bits(0.0));
    CHECK(bits(parameters.depth.max) == bits(0.0));
    CHECK(bits(parameters.temperature.min) == bits(0.5));
    CHECK(bits(parameters.humidity.max) == bits(1.0));
    CHECK(list.entries()[0].biome == ResourceLocation::parse("minecraft:plains"));
}

TEST_CASE("coordinates quantise through float and truncate", "[biome][parameters]") {
    CHECK(quantizeCoord(0.0) == 0);
    CHECK(quantizeCoord(1.0) == 10000);
    CHECK(quantizeCoord(-1.0) == -10000);
    // Truncation towards zero, not rounding: the two sides are not
    // symmetric about a half step, and rounding would move boundaries.
    CHECK(quantizeCoord(0.00019) == 1);
    CHECK(quantizeCoord(-0.00019) == -1);
    // The cast through float is load-bearing, and these are the witnesses:
    // narrowing first lands on a different side of the integer boundary
    // than multiplying in double does, and a different side moves a biome
    // boundary by a ten-thousandth on an axis. The float step is cubiomes'
    // (`sampleBiomeNoise`, from which quantizeCoord is adapted); the goldens
    // score the two spellings alike, so these values are this code's own
    // arithmetic, pinned, and not a measurement of vanilla (SPEC §11).
    CHECK(quantizeCoord(-0.819) == -8190);
    CHECK(static_cast<std::int64_t>(-0.819 * 10000.0) == -8189);
    CHECK(quantizeCoord(-0.8188) == -8187);
    CHECK(static_cast<std::int64_t>(-0.8188 * 10000.0) == -8188);
}

TEST_CASE("distance is zero inside a range and the gap outside", "[biome][parameters]") {
    const Parameter whole{.min = -1.0, .max = 1.0};
    const QuantizedPoint point = QuantizedPoint::of(ParameterPoint{
        .temperature = Parameter{.min = -0.5, .max = 0.5},
        .humidity = whole,
        .continentalness = whole,
        .erosion = whole,
        .depth = whole,
        .weirdness = whole,
        .offset = 0.0,
    });
    const auto at = [&](double temperature) {
        return point.fitness(QuantizedSample::of(ClimateSample{.temperature = temperature}));
    };
    CHECK(at(0.0) == 0);
    CHECK(at(-0.5) == 0);
    CHECK(at(0.5) == 0);
    // Outside, the term is the squared gap in quantised units.
    CHECK(at(1.5) == 10000LL * 10000LL);
    CHECK(at(-1.5) == 10000LL * 10000LL);
}

TEST_CASE("near-misses in the reals are exact ties once quantised", "[biome][parameters]") {
    // This is the whole reason the search is not done in doubles. These two
    // entries are 1e-9 apart on temperature, which in double arithmetic
    // orders them strictly; quantised, they are the same number, and the
    // tie-break decides instead.
    const ParameterList list = parse(nlohmann::json{
        {"biomes",
         {entry("minecraft:first", 0.5, 0.0), entry("minecraft:second", 0.5 + 1e-9, 0.0)}}});
    const ClimateSample sample{.temperature = 0.0};
    CHECK(list.entries()[0].parameters.fitness(sample) ==
          list.entries()[1].parameters.fitness(sample));
    CHECK(list.find(sample) == ResourceLocation::parse("minecraft:second"));
}

TEST_CASE("the search takes the nearest entry, and the later one on a tie", "[biome][parameters]") {
    const ParameterList nearest = parse(nlohmann::json{
        {"biomes", {entry("minecraft:cold", -1.0, 0.0), entry("minecraft:warm", 1.0, 0.0)}}});
    CHECK(nearest.find(ClimateSample{.temperature = -0.9}) ==
          ResourceLocation::parse("minecraft:cold"));
    CHECK(nearest.find(ClimateSample{.temperature = 0.9}) ==
          ResourceLocation::parse("minecraft:warm"));

    // Two entries fit identically. The *later* wins, which makes the answer
    // a function of the table's order as well as its contents — so the order
    // has to survive reading. Measured against vanilla; see the header.
    const ParameterList tied = parse(
        nlohmann::json{{"biomes",
                        {entry("minecraft:cold", -1.0, 0.0), entry("minecraft:warm", 1.0, 0.0),
                         entry("minecraft:also_warm", 1.0, 0.0)}}});
    CHECK(tied.find(ClimateSample{.temperature = 1.0}) ==
          ResourceLocation::parse("minecraft:also_warm"));
    // ...and it is genuinely the later of the tied pair, not just the last
    // row of the table: `cold` is not tied and does not win.
    CHECK(tied.find(ClimateSample{.temperature = -1.0}) ==
          ResourceLocation::parse("minecraft:cold"));
}

TEST_CASE("the offset is a penalty on every distance", "[biome][parameters]") {
    // Same ranges, but one carries an offset. It should lose everywhere,
    // which is what an offset is for: a biome chosen only where nothing
    // else is close.
    const ParameterList list =
        parse(nlohmann::json{{"biomes",
                              {entry("minecraft:penalised", 0.0, 0.0, /*offset=*/0.5),
                               entry("minecraft:plain", 0.0, 0.0)}}});
    CHECK(list.find(ClimateSample{}) == ResourceLocation::parse("minecraft:plain"));
    CHECK(list.find(ClimateSample{.temperature = 0.4}) ==
          ResourceLocation::parse("minecraft:plain"));
}

TEST_CASE("a malformed table is refused by row and axis", "[biome][parameters]") {
    CHECK_THROWS_WITH(parse(nlohmann::json::object()), ContainsSubstring("biomes"));
    CHECK_THROWS_WITH(parse(nlohmann::json{{"biomes", nlohmann::json::object()}}),
                      ContainsSubstring("must be an array"));

    nlohmann::json missingAxis = nlohmann::json{{"biomes", {entry("minecraft:plains", 0.0, 0.0)}}};
    missingAxis["biomes"][0]["parameters"].erase("erosion");
    CHECK_THROWS_WITH(parse(missingAxis), ContainsSubstring("erosion"));

    nlohmann::json backwards = nlohmann::json{{"biomes", {entry("minecraft:plains", 0.0, 0.0)}}};
    backwards["biomes"][0]["parameters"]["humidity"] = nlohmann::json::array({1.0, -1.0});
    CHECK_THROWS_WITH(parse(backwards),
                      ContainsSubstring("humidity") && ContainsSubstring("minimum above"));

    const nlohmann::json badBiome = nlohmann::json{{"biomes", {entry("Not An Id", 0.0, 0.0)}}};
    CHECK_THROWS_WITH(parse(badBiome), ContainsSubstring("entry 0"));

    // An empty table has no right answer, and a world full of one wrong
    // biome is worse than a refusal.
    const ParameterList empty = parse(nlohmann::json{{"biomes", nlohmann::json::array()}});
    CHECK(empty.size() == 0U);
    CHECK_THROWS_AS(empty.find(ClimateSample{}), ParameterError);
}

TEST_CASE("a table bound or offset outside the schema's range is refused",
          "[biome][parameters][ub]") {
    // vanilla-mcdoc: ClimateParameter float @ -2..2, offset float @ 0..1. The
    // server's own tables sit well inside. A bound of 1e15 (or a NaN from a
    // resealed blob) used to reach quantizeCoord, whose int64 conversion is
    // undefined past about 9.2e14 (SPEC §8).
    CHECK_THROWS_WITH(parse(nlohmann::json{{"biomes", {entry("minecraft:plains", 1e15, 0.0)}}}),
                      ContainsSubstring("temperature of entry 0") && ContainsSubstring("[-2, 2]"));
    CHECK_THROWS_WITH(parse(nlohmann::json{{"biomes", {entry("minecraft:plains", 0.0, 0.0, 1.5)}}}),
                      ContainsSubstring("offset of entry 0") && ContainsSubstring("[0, 1]"));
    CHECK_THROWS_WITH(
        parse(nlohmann::json{{"biomes", {entry("minecraft:plains", 0.0, 0.0, -0.25)}}}),
        ContainsSubstring("offset of entry 0"));
    // Both ends of both ranges load.
    CHECK_NOTHROW(parse(nlohmann::json{
        {"biomes",
         {entry("minecraft:plains", 2.0, -2.0, 0.0), entry("minecraft:desert", -2.0, 2.0, 1.0)}}}));

    // fromEntries is what a frozen pipeline goes through, and a blob can
    // carry a NaN that JSON cannot.
    stratum::biome::Entry stored;
    stored.parameters.erosion =
        Parameter{.min = -1.0, .max = std::numeric_limits<double>::quiet_NaN()};
    CHECK_THROWS_WITH(ParameterList::fromEntries({stored}, ResourceLocation::parse("stratum:t")),
                      ContainsSubstring("erosion of entry 0"));
}

TEST_CASE("a climate sample the search cannot hold is refused by name", "[biome][parameters][ub]") {
    // A router entry is any density function: a datapack's temperature of
    // the constant 1e6 — schema-legal — overflowed the fitness sum, and
    // cube(1e6) = 1e18 overflowed quantizeCoord. Both are refused, naming the
    // axis, and the bound itself still searches (SPEC §8).
    const ParameterList list =
        parse(nlohmann::json{{"biomes", {entry("minecraft:plains", 0.5, 0.25)}}});
    ClimateSample sample;
    sample.temperature = 1.0e6;
    CHECK_THROWS_WITH(list.find(sample), ContainsSubstring("temperature of 1000000") &&
                                             ContainsSubstring("Check that router entry"));
    sample.temperature = 0.0;
    sample.erosion = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_WITH(list.find(sample), ContainsSubstring("erosion of nan"));
    sample.erosion = -stratum::biome::kMaxClimateSample;
    sample.temperature = stratum::biome::kMaxClimateSample;
    CHECK(list.find(sample) == ResourceLocation::parse("minecraft:plains"));
}
