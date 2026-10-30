// Stratum — what Executor::compile does for a geometry nothing validated, and
// what apply checks of a Context in a build without NDEBUG.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   * Banding (Executor's y bands) keeps a byte per y of the dimension, so a
//     direct caller's geometry taller than the schema's 4096 is not banded:
//     compile stays proportional to the tree, and every block still gets
//     the list's answer — the bands only ever answer what the list does.
//   * A block handing one of this executor's biome keys beside a biome by
//     name must hand the name's own key: refused, in a build without NDEBUG,
//     where it does not.
#include <stratum/data/resource_location.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::surface::Context;
using stratum::surface::ExecutionError;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

namespace {

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

[[nodiscard]] nlohmann::json when(nlohmann::json test, nlohmann::json then) {
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", std::move(test)},
                          {"then_run", std::move(then)}};
}

[[nodiscard]] nlohmann::json yAbove(std::int32_t anchor) {
    return nlohmann::json{{"type", "minecraft:y_above"},
                          {"anchor", {{"absolute", anchor}}},
                          {"surface_depth_multiplier", 0},
                          {"add_stone_depth", false}};
}

/// A tree a block's y alone decides, and one that also reads the biome.
[[nodiscard]] RuleGraph bandedTree() {
    return RuleGraph::resolve(
        nlohmann::json{
            {"type", "minecraft:sequence"},
            {"sequence",
             {when(yAbove(100), block("stratum:high")),
              when(nlohmann::json{{"type", "minecraft:biome"}, {"biome_is", {"minecraft:desert"}}},
                   block("stratum:sand")),
              when(yAbove(-20), block("stratum:middle")), block("stratum:low")}}},
        ResourceLocation{"stratum", "bounds"});
}

[[nodiscard]] std::string nameOf(const stratum::settings::BlockState* placed) {
    return placed == nullptr ? std::string("nothing") : placed->name.toString();
}

/// What the tree places at @p y by its documented semantics.
[[nodiscard]] std::string expectedAt(std::int32_t y, bool desert) {
    if (y >= 100) {
        return "stratum:high";
    }
    if (desert) {
        return "stratum:sand";
    }
    return y >= -20 ? "stratum:middle" : "stratum:low";
}

} // namespace

TEST_CASE("a geometry taller than the schema's is not banded, and places the same blocks",
          "[surface][program]") {
    const RuleGraph graph = bandedTree();
    const stratum::settings::NoiseGeometry tallest{
        .minY = -2048, .height = 4096, .sizeHorizontal = 1, .sizeVertical = 2};
    const stratum::settings::NoiseGeometry taller{
        .minY = -2048, .height = 4112, .sizeHorizontal = 1, .sizeVertical = 2};
    // Two billion blocks high: banding it would keep two gigabytes.
    const stratum::settings::NoiseGeometry unvalidated{.minY = -1073741824,
                                                       .height =
                                                           std::numeric_limits<std::int32_t>::max(),
                                                       .sizeHorizontal = 1,
                                                       .sizeVertical = 2};
    const Executor banded = Executor::compile(graph, 7, tallest);
    const Executor unbanded = Executor::compile(graph, 7, taller);
    const Executor huge = Executor::compile(graph, 7, unvalidated);
    CHECK(banded.banded());
    CHECK_FALSE(unbanded.banded());
    CHECK_FALSE(huge.banded());
    for (const std::int32_t y : {-3000, -2048, -21, -20, 0, 99, 100, 2047, 2063, 5000}) {
        for (const bool desert : {false, true}) {
            CAPTURE(y, desert);
            Context at;
            at.y = y;
            at.biome = ResourceLocation::parse(desert ? "minecraft:desert" : "minecraft:plains");
            const std::string expected = expectedAt(y, desert);
            CHECK(nameOf(banded.apply(at)) == expected);
            CHECK(nameOf(unbanded.apply(at)) == expected);
            CHECK(nameOf(huge.apply(at)) == expected);
            // And by key alone, the keyed copies where there are bands.
            Context keyed = at;
            keyed.biome.reset();
            keyed.biomeKey = banded.biomeKey(*at.biome);
            CHECK(nameOf(banded.apply(keyed)) == expected);
            keyed.biomeKey = huge.biomeKey(*at.biome);
            CHECK(nameOf(huge.apply(keyed)) == expected);
        }
    }
}

TEST_CASE("a key beside a name must be the name's own key", "[surface][biome]") {
    const RuleGraph graph = bandedTree();
    const stratum::settings::NoiseGeometry geometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
    const Executor executor = Executor::compile(graph, 7, geometry);
    Context at;
    at.y = 0;
    at.biome = ResourceLocation::parse("minecraft:plains");
    // The name's own key beside it: answered.
    at.biomeKey = executor.biomeKey(*at.biome);
    CHECK(nameOf(executor.apply(at)) == "stratum:middle");
    // Another biome's key beside it.
    at.biomeKey = executor.biomeKey(ResourceLocation::parse("minecraft:desert"));
#if !defined(NDEBUG)
    CHECK_THROWS_AS(executor.apply(at), ExecutionError);
    CHECK_THROWS_WITH(executor.apply(at), ContainsSubstring("must be the name's own"));
    stratum::surface::ColumnMemo memo;
    CHECK_THROWS_WITH(executor.apply(at, memo), ContainsSubstring("'minecraft:plains'"));
#else
    // Unchecked where NDEBUG is set: the key decides, as its doc says.
    CHECK(nameOf(executor.apply(at)) == "stratum:sand");
#endif
    // Another executor's key is not this one's to check: the name answers.
    const Executor other = Executor::compile(graph, 7, geometry);
    at.biomeKey = other.biomeKey(ResourceLocation::parse("minecraft:desert"));
    CHECK(nameOf(executor.apply(at)) == "stratum:middle");
}
