// Stratum — the generated Java block state -> Bedrock blockstate table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free: whether every vanilla state resolves, and whether ids match
// vanilla's own report, needs `tools/fetch-vanilla`'s data and lives in
// tests/conformance/vanilla_block_state_mapping_test.cpp. What belongs here
// is the loader's own behaviour against values read directly out of
// GeyserMC's blocks.nbt and vanilla's report (not recomputed by the same
// logic that generated the table), and the shape of every refusal.
#include <stratum/mapping/block_state.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::mapping::bedrockBlockState;
using stratum::mapping::BedrockBlockState;
using stratum::mapping::bedrockBlockStateVersion;
using stratum::mapping::BedrockStateValue;
using stratum::mapping::BedrockTagType;
using stratum::mapping::javaBlockStateCount;
using stratum::mapping::javaBlockStateId;
using stratum::settings::BlockState;

namespace {

[[nodiscard]] BlockState java(std::string_view name,
                              std::map<std::string, std::string> properties = {}) {
    return BlockState{.name = ResourceLocation::parse(name), .properties = std::move(properties)};
}

[[nodiscard]] BedrockStateValue byteValue(std::string_view name, std::int32_t value) {
    return {.name = name, .type = BedrockTagType::Byte, .integer = value, .string = {}};
}

[[nodiscard]] BedrockStateValue intValue(std::string_view name, std::int32_t value) {
    return {.name = name, .type = BedrockTagType::Int, .integer = value, .string = {}};
}

[[nodiscard]] BedrockStateValue stringValue(std::string_view name, std::string_view value) {
    return {.name = name, .type = BedrockTagType::String, .integer = 0, .string = value};
}

constexpr std::int32_t kVersion1_21_60_33 = (1 << 24) | (21 << 16) | (60 << 8) | 33;

} // namespace

TEST_CASE("Java state ids follow vanilla's own registry numbering", "[mapping]") {
    CHECK(javaBlockStateId(java("minecraft:air")) == 0);
    CHECK(javaBlockStateId(java("minecraft:stone")) == 1);
    CHECK(javaBlockStateId(java("minecraft:water", {{"level", "1"}})) == 87);
    CHECK(javaBlockStateId(java("minecraft:oak_log", {{"axis", "x"}})) == 136);
    CHECK(javaBlockStateId(java("minecraft:snow_block")) == 6727);
    CHECK(javaBlockStateId(java("minecraft:terracotta")) == 12710);
}

TEST_CASE("omitted properties take the block's default, as in a data pack", "[mapping]") {
    CHECK(javaBlockStateId(java("minecraft:grass_block")) == 9);
    CHECK(javaBlockStateId(java("minecraft:grass_block", {{"snowy", "false"}})) == 9);
    CHECK(javaBlockStateId(java("minecraft:grass_block", {{"snowy", "true"}})) == 8);
    CHECK(javaBlockStateId(java("minecraft:snow")) == 6718);
}

TEST_CASE("a block whose ids are not numbered in its report's property order", "[mapping]") {
    // vanilla's report lists chest's properties as type, facing, waterlogged,
    // but numbers its states facing-major — one of 12 such blocks at 1.21.11.
    CHECK(javaBlockStateId(java("minecraft:chest")) == 3787);
    CHECK(javaBlockStateId(java("minecraft:chest",
                                {{"facing", "east"}, {"type", "left"}, {"waterlogged", "true"}})) ==
          3806);
}

TEST_CASE("Bedrock states carry the names, tag types and version GeyserMC gives", "[mapping]") {
    CHECK(bedrockBlockState(java("minecraft:air")) ==
          BedrockBlockState{.name = "minecraft:air", .states = {}, .version = kVersion1_21_60_33});
    CHECK(bedrockBlockState(java("minecraft:grass_block")) ==
          BedrockBlockState{
              .name = "minecraft:grass_block", .states = {}, .version = kVersion1_21_60_33});
    // Renamed on Bedrock.
    CHECK(bedrockBlockState(java("minecraft:terracotta")).name == "minecraft:hardened_clay");
    CHECK(bedrockBlockState(java("minecraft:snow_block")).name == "minecraft:snow");
    // Byte vs Int vs String are different states to the consuming platforms.
    CHECK(bedrockBlockState(java("minecraft:snow")) ==
          BedrockBlockState{.name = "minecraft:snow_layer",
                            .states = {byteValue("covered_bit", 0), intValue("height", 0)},
                            .version = kVersion1_21_60_33});
    CHECK(bedrockBlockState(java("minecraft:bedrock")).states ==
          std::vector{byteValue("infiniburn_bit", 0)});
    CHECK(bedrockBlockState(java("minecraft:water", {{"level", "1"}})) ==
          BedrockBlockState{.name = "minecraft:flowing_water",
                            .states = {intValue("liquid_depth", 1)},
                            .version = kVersion1_21_60_33});
    CHECK(bedrockBlockState(java("minecraft:oak_log", {{"axis", "x"}})).states ==
          std::vector{stringValue("pillar_axis", "x")});
    CHECK(bedrockBlockState(java("minecraft:chest", {{"facing", "east"}})).states ==
          std::vector{stringValue("minecraft:cardinal_direction", "east")});
}

TEST_CASE("the table covers every state at one blockstate version", "[mapping]") {
    CHECK(bedrockBlockStateVersion() == kVersion1_21_60_33);
    REQUIRE(javaBlockStateCount() > 0);
    const auto last = static_cast<std::uint32_t>(javaBlockStateCount() - 1);
    CHECK_FALSE(bedrockBlockState(last).name.empty());
}

TEST_CASE("a state the table cannot place is refused by name, never guessed", "[mapping]") {
    CHECK_THROWS_WITH(javaBlockStateId(java("minecraft:not_a_block")),
                      ContainsSubstring("'minecraft:not_a_block' is not a block"));
    CHECK_THROWS_WITH(javaBlockStateId(java("examplemod:custom_block")),
                      ContainsSubstring("'examplemod:custom_block' is not a block"));
    CHECK_THROWS_WITH(javaBlockStateId(java("minecraft:oak_log", {{"facing", "north"}})),
                      ContainsSubstring("'minecraft:oak_log' has no property 'facing'") &&
                          ContainsSubstring("it has: axis"));
    CHECK_THROWS_WITH(javaBlockStateId(java("minecraft:stone", {{"axis", "x"}})),
                      ContainsSubstring("it has: none"));
    CHECK_THROWS_WITH(javaBlockStateId(java("minecraft:oak_log", {{"axis", "w"}})),
                      ContainsSubstring("property 'axis' has no value 'w'") &&
                          ContainsSubstring("it has: x, y, z"));
    CHECK_THROWS_WITH(bedrockBlockState(static_cast<std::uint32_t>(javaBlockStateCount())),
                      ContainsSubstring("out of range"));
}
