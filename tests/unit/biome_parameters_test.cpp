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
#include <string>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::biome::ClimateSample;
using stratum::biome::Parameter;
using stratum::biome::ParameterError;
using stratum::biome::ParameterList;
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

TEST_CASE("distance is zero inside a range and the gap outside", "[biome][parameters]") {
    const Parameter parameter{.min = -0.5, .max = 0.5};
    CHECK(bits(parameter.distanceTo(0.0)) == bits(0.0));
    CHECK(bits(parameter.distanceTo(-0.5)) == bits(0.0));
    CHECK(bits(parameter.distanceTo(0.5)) == bits(0.0));
    CHECK(bits(parameter.distanceTo(1.5)) == bits(1.0));
    CHECK(bits(parameter.distanceTo(-1.5)) == bits(1.0));
}

TEST_CASE("the search takes the nearest entry, and the earlier one on a tie",
          "[biome][parameters]") {
    const ParameterList list = parse(
        nlohmann::json{{"biomes",
                        {entry("minecraft:cold", -1.0, 0.0), entry("minecraft:warm", 1.0, 0.0),
                         entry("minecraft:also_warm", 1.0, 0.0)}}});

    CHECK(list.find(ClimateSample{.temperature = -0.9}) ==
          ResourceLocation::parse("minecraft:cold"));
    CHECK(list.find(ClimateSample{.temperature = 0.9}) ==
          ResourceLocation::parse("minecraft:warm"));
    // Two entries fit identically. The earlier wins, which makes the answer
    // a function of the table's order as well as its contents — so the order
    // has to survive reading.
    CHECK(list.find(ClimateSample{.temperature = 1.0}) ==
          ResourceLocation::parse("minecraft:warm"));
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

    nlohmann::json badBiome = nlohmann::json{{"biomes", {entry("Not An Id", 0.0, 0.0)}}};
    CHECK_THROWS_WITH(parse(badBiome), ContainsSubstring("entry 0"));

    // An empty table has no right answer, and a world full of one wrong
    // biome is worse than a refusal.
    const ParameterList empty = parse(nlohmann::json{{"biomes", nlohmann::json::array()}});
    CHECK(empty.size() == 0U);
    CHECK_THROWS_AS(empty.find(ClimateSample{}), ParameterError);
}
