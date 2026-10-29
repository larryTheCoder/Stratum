// Stratum — a biome handed to the surface rules as the executor's own key.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// surface::Executor::biomeKey turns a biome into an index the `biome`
// conditions answer from a table compile() filled, instead of comparing the
// biome's name against every name a condition lists. These cases hold the
// key to the name: the same block at every position, for every biome a
// condition names and for ones none does, and a key is only ever read by the
// executor that issued it.
#include <stratum/data/resource_location.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::surface::Context;
using stratum::surface::ExecutionError;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

[[nodiscard]] stratum::settings::NoiseGeometry geometry() {
    return stratum::settings::NoiseGeometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

[[nodiscard]] nlohmann::json biomeIs(const std::vector<std::string>& biomes) {
    return nlohmann::json{{"type", "minecraft:biome"}, {"biome_is", biomes}};
}

[[nodiscard]] nlohmann::json when(const nlohmann::json& condition, const nlohmann::json& then) {
    return nlohmann::json{
        {"type", "minecraft:condition"}, {"if_true", condition}, {"then_run", then}};
}

/// Biome conditions over overlapping lists, one negated, one nested under
/// another and one behind a y test, so that a block's answer depends on
/// which biome it is in, which lists name it, and where it sits.
[[nodiscard]] RuleGraph biomeTree() {
    const nlohmann::json json{
        {"type", "minecraft:sequence"},
        {"sequence",
         {when(nlohmann::json{{"type", "minecraft:y_above"},
                              {"anchor", {{"absolute", 40}}},
                              {"surface_depth_multiplier", 0},
                              {"add_stone_depth", false}},
               when(biomeIs({"minecraft:desert", "minecraft:badlands"}), block("minecraft:sand"))),
          when(biomeIs({"minecraft:plains"}), block("minecraft:grass_block")),
          when(biomeIs({"minecraft:badlands", "minecraft:eroded_badlands"}),
               when(biomeIs({"minecraft:eroded_badlands"}), block("minecraft:terracotta"))),
          when(nlohmann::json{{"type", "minecraft:not"},
                              {"invert", biomeIs({"minecraft:desert", "minecraft:plains"})}},
               block("minecraft:stone")),
          when(biomeIs({"minecraft:desert"}), block("minecraft:sandstone"))}}};
    return RuleGraph::resolve(json, ResourceLocation::parse("stratum:test"));
}

[[nodiscard]] Context at(std::int32_t y) {
    Context context;
    context.x = 3;
    context.y = y;
    context.z = -7;
    return context;
}

} // namespace

TEST_CASE("a biome key answers every biome condition as the biome's name does",
          "[surface][biome]") {
    const RuleGraph graph = biomeTree();
    const auto dimension = geometry();
    const Executor executor = Executor::compile(graph, 42, dimension);
    // Every name a condition lists, and three no condition does, which all
    // share one key.
    const std::vector<std::string> biomes{
        "minecraft:desert", "minecraft:badlands", "minecraft:eroded_badlands", "minecraft:plains",
        "minecraft:forest", "minecraft:the_void", "stratum:not_a_vanilla"};
    std::set<std::string> placed;
    for (const std::string& name : biomes) {
        const ResourceLocation biome = ResourceLocation::parse(name);
        for (const std::int32_t y : {-64, 0, 39, 40, 41, 200}) {
            CAPTURE(name, y);
            Context byName = at(y);
            byName.biome = biome;
            const auto* expected = executor.apply(byName);

            // The key alone: the name is not read at all.
            Context byKey = at(y);
            byKey.biomeKey = executor.biomeKey(biome);
            CHECK(executor.apply(byKey) == expected);

            // Both, as a caller holding both would hand them over.
            byName.biomeKey = executor.biomeKey(biome);
            CHECK(executor.apply(byName) == expected);

            REQUIRE(expected != nullptr);
            placed.insert(expected->name.toString());
        }
    }
    // All five of the tree's blocks were placed, so the keys decided real
    // blocks.
    CHECK(placed.size() == 5U);
    // Biomes no condition names share a key; named ones each have their own.
    CHECK(executor.biomeKey(ResourceLocation::parse("minecraft:forest")).index ==
          executor.biomeKey(ResourceLocation::parse("stratum:not_a_vanilla")).index);
    CHECK(executor.biomeKey(ResourceLocation::parse("minecraft:desert")).index !=
          executor.biomeKey(ResourceLocation::parse("minecraft:plains")).index);
}

TEST_CASE("a biome key is read only by the executor that issued it, or a copy of it",
          "[surface][biome]") {
    const RuleGraph graph = biomeTree();
    const auto dimension = geometry();
    const Executor issuer = Executor::compile(graph, 42, dimension);
    const Executor other = Executor::compile(graph, 42, dimension);
    const Executor copy = issuer; // NOLINT(performance-unnecessary-copy-initialization)
    const ResourceLocation plains = ResourceLocation::parse("minecraft:plains");

    Context keyOnly = at(0);
    keyOnly.biomeKey = issuer.biomeKey(plains);
    REQUIRE(issuer.apply(keyOnly) != nullptr);
    CHECK(issuer.apply(keyOnly)->name.toString() == "minecraft:grass_block");
    CHECK(copy.apply(keyOnly) == issuer.apply(keyOnly));

    // Another compile, even of the same tree, ignores the key: with no biome
    // beside it the block is refused as a keyless one always was...
    CHECK_THROWS_AS(other.apply(keyOnly), ExecutionError);
    CHECK_THROWS_WITH(other.apply(keyOnly), ContainsSubstring("without a biome"));
    // ...and with one it is answered by the name, never by the key.
    Context mismatched = at(0);
    mismatched.biome = ResourceLocation::parse("minecraft:desert");
    mismatched.biomeKey = issuer.biomeKey(plains);
    REQUIRE(other.apply(mismatched) != nullptr);
    CHECK(other.apply(mismatched)->name.toString() == "minecraft:sandstone");

    // No key at all is the default, and is the name's.
    const Context none;
    CHECK(none.biomeKey.executor == 0U);
}

TEST_CASE("a biome key altered past the executor's table is refused, not read",
          "[surface][biome]") {
    const RuleGraph graph = biomeTree();
    const auto dimension = geometry();
    const Executor executor = Executor::compile(graph, 42, dimension);
    Context forged = at(0);
    forged.biomeKey = executor.biomeKey(ResourceLocation::parse("minecraft:forest"));
    forged.biomeKey.index += 1U;
    CHECK_THROWS_AS(executor.apply(forged), ExecutionError);
    CHECK_THROWS_WITH(executor.apply(forged), ContainsSubstring("never issued"));
}
