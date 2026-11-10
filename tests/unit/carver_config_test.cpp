// Stratum — configured carvers, block tags and a biome's carvers field, as
// parsed, against synthetic data.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/carver/config.hpp>
#include <stratum/data/block_tags.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::carver::CarverError;
using stratum::carver::CarverType;
using stratum::carver::ConfiguredCarver;
using stratum::carver::DebugSettings;
using stratum::carver::FloatProvider;
using stratum::carver::HeightProvider;
using stratum::data::BlockTagEntry;
using stratum::data::BlockTagError;
using stratum::data::BlockTags;
using stratum::data::ResourceLocation;

namespace {

[[nodiscard]] std::uint32_t bits(const float value) {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] ResourceLocation id(const char* text) {
    return ResourceLocation::parse(text);
}

[[nodiscard]] BlockTagEntry block(const char* text) {
    return BlockTagEntry{.id = id(text), .isTag = false, .required = true};
}

[[nodiscard]] BlockTagEntry tag(const char* text, bool required = true) {
    return BlockTagEntry{.id = id(text), .isTag = true, .required = required};
}

[[nodiscard]] nlohmann::json caveJson() {
    return nlohmann::json::parse(R"({"type": "minecraft:cave", "config": {
        "probability": 0.15,
        "y": {"type": "minecraft:uniform", "min_inclusive": {"above_bottom": 8},
              "max_inclusive": {"absolute": 180}},
        "yScale": {"type": "minecraft:uniform", "min_inclusive": 0.1, "max_exclusive": 0.9},
        "lava_level": {"above_bottom": 8},
        "replaceable": "#minecraft:overworld_carver_replaceables",
        "horizontal_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.7,
                                         "max_exclusive": 1.4},
        "vertical_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.8,
                                       "max_exclusive": 1.3},
        "floor_level": {"type": "minecraft:uniform", "min_inclusive": -1.0,
                        "max_exclusive": -0.4},
        "debug_settings": {"air_state": {"Name": "minecraft:oak_button"}}}})");
}

} // namespace

TEST_CASE("a block tag resolves through the tags it references", "[carver][tags]") {
    const BlockTags tags = BlockTags::fromEntries({
        {id("minecraft:replaceables"),
         {block("minecraft:water"), tag("minecraft:stones"), tag("minecraft:absent", false)}},
        {id("minecraft:stones"), {block("minecraft:stone"), tag("minecraft:more")}},
        {id("minecraft:more"), {block("minecraft:granite"), block("minecraft:stone")}},
    });
    CHECK(tags.resolve(id("minecraft:replaceables")) ==
          std::set<ResourceLocation>{id("minecraft:water"), id("minecraft:stone"),
                                     id("minecraft:granite")});
}

TEST_CASE("a block tag that cannot resolve is refused by name", "[carver][tags]") {
    const BlockTags missing =
        BlockTags::fromEntries({{id("minecraft:a"), {tag("minecraft:nowhere")}}});
    CHECK_THROWS_MATCHES(missing.resolve(id("minecraft:a")), BlockTagError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("#minecraft:nowhere")));
    CHECK_THROWS_AS(missing.resolve(id("minecraft:b")), BlockTagError);

    const BlockTags cycle = BlockTags::fromEntries({
        {id("minecraft:a"), {tag("minecraft:b")}},
        {id("minecraft:b"), {tag("minecraft:a")}},
    });
    CHECK_THROWS_MATCHES(cycle.resolve(id("minecraft:a")), BlockTagError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("cycle")));
}

TEST_CASE("a configured cave parses every field, each provider form included", "[carver][config]") {
    const ConfiguredCarver cave = ConfiguredCarver::fromJson(caveJson(), "minecraft:cave");
    CHECK(cave.type == CarverType::Cave);
    // The pack's float: 0.15 is stored as its float rounding (spec A-Q1.3).
    CHECK(bits(cave.probability) == bits(0.15F));
    CHECK(cave.y.kind == HeightProvider::Kind::Uniform);
    CHECK(cave.y.minInclusive.kind == stratum::surface::VerticalAnchor::Kind::AboveBottom);
    CHECK(cave.y.minInclusive.value == 8);
    CHECK(cave.y.maxInclusive.value == 180);
    CHECK(cave.yScale.kind == FloatProvider::Kind::Uniform);
    CHECK(bits(cave.yScale.a) == bits(0.1F));
    CHECK(bits(cave.yScale.b) == bits(0.9F));
    CHECK(cave.replaceable.isTag);
    CHECK(cave.replaceable.tag == id("minecraft:overworld_carver_replaceables"));
    CHECK(bits(cave.floorLevel.a) == bits(-1.0F));
    CHECK(bits(cave.floorLevel.b) == bits(-0.4F));

    const nlohmann::json nether =
        nlohmann::json::parse(R"({"type": "minecraft:nether_cave", "config": {
        "probability": 0.2, "floor_level": -0.7, "horizontal_radius_multiplier": 1.0,
        "vertical_radius_multiplier": 1.0, "lava_level": {"above_bottom": 10},
        "replaceable": ["minecraft:netherrack", "minecraft:basalt"],
        "y": {"type": "minecraft:uniform", "min_inclusive": {"absolute": 0},
              "max_inclusive": {"below_top": 1}},
        "yScale": 0.5}})");
    const ConfiguredCarver netherCave = ConfiguredCarver::fromJson(nether, "minecraft:nether_cave");
    CHECK(netherCave.type == CarverType::NetherCave);
    CHECK_FALSE(netherCave.yScale.draws());
    CHECK(bits(netherCave.yScale.a) == bits(0.5F));
    CHECK_FALSE(netherCave.replaceable.isTag);
    CHECK(netherCave.replaceable.blocks ==
          std::vector<ResourceLocation>{id("minecraft:netherrack"), id("minecraft:basalt")});
}

TEST_CASE("a configured canyon parses its shape, the trapezoid included", "[carver][config]") {
    const nlohmann::json json = nlohmann::json::parse(R"({"type": "minecraft:canyon", "config": {
        "probability": 0.01, "lava_level": {"above_bottom": 8},
        "replaceable": "minecraft:stone",
        "y": {"type": "minecraft:uniform", "min_inclusive": {"absolute": 10},
              "max_inclusive": {"absolute": 67}},
        "yScale": 3.0,
        "vertical_rotation": {"type": "minecraft:uniform", "min_inclusive": -0.125,
                              "max_exclusive": 0.125},
        "shape": {
            "distance_factor": {"type": "minecraft:uniform", "min_inclusive": 0.75,
                                "max_exclusive": 1.0},
            "thickness": {"type": "minecraft:trapezoid", "min": 0.0, "max": 6.0, "plateau": 2.0},
            "width_smoothness": 3,
            "horizontal_radius_factor": {"type": "minecraft:uniform", "min_inclusive": 0.75,
                                         "max_exclusive": 1.0},
            "vertical_radius_default_factor": 1.0,
            "vertical_radius_center_factor": 0.0}}})");
    const ConfiguredCarver canyon = ConfiguredCarver::fromJson(json, "minecraft:canyon");
    CHECK(canyon.type == CarverType::Canyon);
    CHECK(canyon.shape.thickness.kind == FloatProvider::Kind::Trapezoid);
    CHECK(bits(canyon.shape.thickness.a) == bits(0.0F));
    CHECK(bits(canyon.shape.thickness.b) == bits(6.0F));
    CHECK(bits(canyon.shape.thickness.c) == bits(2.0F));
    CHECK(canyon.shape.widthSmoothness == 3);
    CHECK(canyon.replaceable.blocks == std::vector<ResourceLocation>{id("minecraft:stone")});
}

TEST_CASE("debug_settings is read in full, each omitted state falling back to the button",
          "[carver][config]") {
    // spec/carvers-a-spec.md Q13.4. The button's own state is the server's:
    // the debug golden stores it where a carver's air state was left out.
    const stratum::settings::BlockState button{
        .name = id("minecraft:acacia_button"),
        .properties = {{"face", "wall"}, {"facing", "north"}, {"powered", "false"}}};

    SECTION("omitted entirely: debug off, the four defaults") {
        nlohmann::json json = caveJson();
        json["config"].erase("debug_settings");
        const ConfiguredCarver cave = ConfiguredCarver::fromJson(json, "stratum:cave");
        CHECK(cave.debug == DebugSettings::omitted());
        CHECK_FALSE(cave.debug.debugMode);
        CHECK(cave.debug.airState == button);
        CHECK(cave.debug.waterState.name == id("minecraft:candle"));
        CHECK(cave.debug.waterState.properties.at("waterlogged") == "false");
        CHECK(cave.debug.lavaState.name == id("minecraft:orange_stained_glass"));
        CHECK(cave.debug.barrierState.name == id("minecraft:glass"));
    }
    SECTION("present: a state left out is the button, whichever it is") {
        // caveJson() gives an air state alone.
        const ConfiguredCarver cave = ConfiguredCarver::fromJson(caveJson(), "stratum:cave");
        CHECK_FALSE(cave.debug.debugMode);
        CHECK(cave.debug.airState.name == id("minecraft:oak_button"));
        CHECK(cave.debug.airState.properties.empty());
        CHECK(cave.debug.waterState == button);
        CHECK(cave.debug.lavaState == button);
        CHECK(cave.debug.barrierState == button);
    }
    SECTION("debug_mode on, with properties") {
        nlohmann::json json = caveJson();
        json["config"]["debug_settings"] = {
            {"debug_mode", true},
            {"water_state",
             {{"Name", "minecraft:oak_slab"},
              {"Properties", {{"type", "bottom"}, {"waterlogged", "false"}}}}}};
        const ConfiguredCarver cave = ConfiguredCarver::fromJson(json, "stratum:cave");
        CHECK(cave.debug.debugMode);
        CHECK(cave.debug.airState == button);
        CHECK(cave.debug.waterState.name == id("minecraft:oak_slab"));
        // As written: the carver sets `waterlogged` when it writes the state.
        CHECK(cave.debug.waterState.properties.at("waterlogged") == "false");
        CHECK(cave.debug.waterState.properties.at("type") == "bottom");
    }
}

TEST_CASE("what this build cannot carve is refused by name", "[carver][config]") {
    nlohmann::json debugMode = caveJson();
    debugMode["config"]["debug_settings"]["debug_mode"] = "yes";
    CHECK_THROWS_MATCHES(ConfiguredCarver::fromJson(debugMode, "stratum:debug"), CarverError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("stratum:debug") &&
                                                         ContainsSubstring("debug_mode")));
    nlohmann::json debugState = caveJson();
    debugState["config"]["debug_settings"]["lava_state"] = "minecraft:glass";
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(debugState, "stratum:x"),
                      ContainsSubstring("debug_settings.lava_state"));
    debugState["config"]["debug_settings"]["lava_state"] = {{"Name", "minecraft:glass"},
                                                            {"Properties", {{"lit", true}}}};
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(debugState, "stratum:x"),
                      ContainsSubstring("\"lit\""));
    debugState["config"]["debug_settings"]["lava_state"] = {{"Name", "minecraft:glass"},
                                                            {"Extra", 1}};
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(debugState, "stratum:x"),
                      ContainsSubstring("Extra"));

    nlohmann::json type = caveJson();
    type["type"] = "stratum:tunnel";
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(type, "stratum:x"),
                      ContainsSubstring("stratum:tunnel"));

    nlohmann::json provider = caveJson();
    provider["config"]["yScale"] = {{"type", "minecraft:clamped_normal"}};
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(provider, "stratum:x"),
                      ContainsSubstring("minecraft:clamped_normal"));

    nlohmann::json extra = caveJson();
    extra["config"]["vertical_rotation"] = 0.0;
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(extra, "stratum:x"),
                      ContainsSubstring("vertical_rotation"));

    nlohmann::json height = caveJson();
    height["config"]["y"] = {{"type", "minecraft:biased_to_bottom"}};
    CHECK_THROWS_WITH(ConfiguredCarver::fromJson(height, "stratum:x"),
                      ContainsSubstring("minecraft:biased_to_bottom"));
}

TEST_CASE("a biome's carvers field is a bare id, a list, or inline definitions",
          "[carver][config]") {
    using stratum::carver::CarverEntry;
    using stratum::carver::parseBiomeCarvers;
    // spec/carvers-b-spec.md Q11.1: the shipped Nether biomes write a bare id.
    const std::vector<CarverEntry> bare =
        parseBiomeCarvers(nlohmann::json("minecraft:nether_cave"), "minecraft:nether_wastes");
    REQUIRE(bare.size() == 1);
    CHECK(std::get<ResourceLocation>(bare.front()) == id("minecraft:nether_cave"));

    const nlohmann::json list = nlohmann::json::array({"minecraft:cave", caveJson()});
    const std::vector<CarverEntry> mixed = parseBiomeCarvers(list, "stratum:plains");
    REQUIRE(mixed.size() == 2);
    CHECK(std::holds_alternative<ResourceLocation>(mixed.at(0)));
    CHECK(std::get<ConfiguredCarver>(mixed.at(1)).type == CarverType::Cave);

    CHECK(parseBiomeCarvers(nlohmann::json::array(), "minecraft:the_end").empty());
    CHECK_THROWS_WITH(parseBiomeCarvers(nlohmann::json("#minecraft:caves"), "stratum:b"),
                      ContainsSubstring("#minecraft:caves"));
    CHECK_THROWS_AS(parseBiomeCarvers(nlohmann::json::object(), "stratum:b"), CarverError);
}
