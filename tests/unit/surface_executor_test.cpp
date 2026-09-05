// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/data/pack.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::surface::Context;
using stratum::surface::ExecutionError;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

namespace {

constexpr std::int64_t kSeed = 42;

[[nodiscard]] stratum::settings::NoiseGeometry overworldGeometry() {
    return stratum::settings::NoiseGeometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
}

[[nodiscard]] RuleGraph resolve(const nlohmann::json& json) {
    return RuleGraph::resolve(json, stratum::data::ResourceLocation::parse("stratum:test"));
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

/// Spelled out rather than using a designated initialiser: the project set
/// builds with -Werror=missing-field-initializers, and a Context grows fields
/// as conditions are settled, so every call site would have to grow with it.
[[nodiscard]] Context at(std::int32_t x, std::int32_t y, std::int32_t z,
                         std::int32_t preliminarySurface = 0) {
    Context context;
    context.x = x;
    context.y = y;
    context.z = z;
    context.preliminarySurface = preliminarySurface;
    return context;
}

[[nodiscard]] nlohmann::json gradient(const std::string& randomName, int trueAt, int falseAt) {
    return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                          {"random_name", randomName},
                          {"true_at_and_below", {{"absolute", trueAt}}},
                          {"false_at_and_above", {{"absolute", falseAt}}}};
}

} // namespace

TEST_CASE("a tree with an unrunnable construct is refused whole, by name", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph = resolve(nlohmann::json{
        {"type", "minecraft:sequence"},
        {"sequence",
         {nlohmann::json{{"type", "minecraft:condition"},
                         {"if_true", gradient("minecraft:deepslate", -8, 8)},
                         {"then_run", block("minecraft:deepslate")}},
          nlohmann::json{{"type", "minecraft:condition"},
                         {"if_true", nlohmann::json{{"type", "minecraft:biome"},
                                                    {"biome_is",
                                                     nlohmann::json::array({"minecraft:plains"})}}},
                         {"then_run", block("minecraft:stone")}}}}});

    // The runnable half is not a licence to run the tree: one unrunnable
    // branch refuses all of it, and the message says which and why.
    CHECK_THROWS_WITH(Executor::compile(graph, kSeed, geometry),
                      ContainsSubstring("minecraft:biome") && ContainsSubstring("refused"));
}

TEST_CASE("a runnable tree places what its rules say", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", gradient("minecraft:deepslate", -8, 8)},
                               {"then_run", block("minecraft:deepslate")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    // Below the lower anchor the gradient is certain, above the upper one it
    // is impossible — no draw is consulted at either end.
    CHECK(executor.apply(at(0, -8, 0)) != nullptr);
    CHECK(executor.apply(at(0, -64, 0)) != nullptr);
    CHECK(executor.apply(at(0, 8, 0)) == nullptr);
    CHECK(executor.apply(at(0, 100, 0)) == nullptr);

    const auto* placed = executor.apply(at(0, -8, 0));
    REQUIRE(placed != nullptr);
    CHECK(placed->name == stratum::data::ResourceLocation::parse("minecraft:deepslate"));
}

TEST_CASE("a sequence stops at the first rule that places something", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph = resolve(
        nlohmann::json{{"type", "minecraft:sequence"},
                       {"sequence", {block("minecraft:granite"), block("minecraft:andesite")}}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    const auto* placed = executor.apply(at(0, 0, 0));
    REQUIRE(placed != nullptr);
    CHECK(placed->name == stratum::data::ResourceLocation::parse("minecraft:granite"));
}

TEST_CASE("a condition that does not hold places nothing at all", "[surface]") {
    const auto geometry = overworldGeometry();
    // `not` over a gradient that is certain low down, so the whole thing is
    // false low down and true high up.
    const RuleGraph graph = resolve(nlohmann::json{
        {"type", "minecraft:condition"},
        {"if_true",
         {{"type", "minecraft:not"}, {"invert", gradient("minecraft:deepslate", -8, 8)}}},
        {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    CHECK(executor.apply(at(0, -32, 0)) == nullptr);
    CHECK(executor.apply(at(0, 32, 0)) != nullptr);
}

TEST_CASE("above_preliminary_surface reads the column's own level", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", {{"type", "minecraft:above_preliminary_surface"}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    CHECK(executor.apply(at(0, 70, 0, 64)) != nullptr);
    CHECK(executor.apply(at(0, 60, 0, 64)) == nullptr);

    // The boundary itself is the DOCUMENTED reading and not a measured one:
    // the only probe that reached this condition found it true everywhere,
    // which cannot separate >= from > (SPEC §11). If that is ever settled the
    // other way, this is the assertion that has to change.
    CHECK(executor.apply(at(0, 64, 0, 64)) != nullptr);
}

TEST_CASE("stone_depth counts from a run's own edge, not from the world's", "[surface]") {
    const auto geometry = overworldGeometry();
    const auto rule = [](const char* type, int offset) {
        return nlohmann::json{{"type", "minecraft:condition"},
                              {"if_true", nlohmann::json{{"type", "minecraft:stone_depth"},
                                                         {"offset", offset},
                                                         {"add_surface_depth", false},
                                                         {"secondary_depth_range", 0},
                                                         {"surface_type", type}}},
                              {"then_run", block("minecraft:stone")}};
    };

    // offset 0 paints one block, offset 2 paints three: the comparison is
    // depth <= offset on a 0-BASED depth, where the stored counter is 1 at a
    // run's top. A strict test, or a 1-based depth, moves every surface layer.
    const RuleGraph topGraph = resolve(rule("floor", 0));
    const Executor top = Executor::compile(topGraph, kSeed, geometry);
    Context inRun = at(0, 100, 0);
    inRun.stoneDepthAbove = 1;
    CHECK(top.apply(inRun) != nullptr);
    inRun.stoneDepthAbove = 2;
    CHECK(top.apply(inRun) == nullptr);

    const RuleGraph threeGraph = resolve(rule("floor", 2));
    const Executor three = Executor::compile(threeGraph, kSeed, geometry);
    for (std::int32_t depth = 1; depth <= 3; ++depth) {
        Context ctx = at(0, 100, 0);
        ctx.stoneDepthAbove = depth;
        CHECK(three.apply(ctx) != nullptr);
    }
    Context past = at(0, 100, 0);
    past.stoneDepthAbove = 4;
    CHECK(three.apply(past) == nullptr);

    // "ceiling" reads the other counter, so a block deep under a run's top can
    // still be at its bottom.
    const RuleGraph bottomGraph = resolve(rule("ceiling", 0));
    const Executor bottom = Executor::compile(bottomGraph, kSeed, geometry);
    Context deep = at(0, 100, 0);
    deep.stoneDepthAbove = 40;
    deep.stoneDepthBelow = 1;
    CHECK(bottom.apply(deep) != nullptr);
}

TEST_CASE("water is unconditionally true in a column with no fluid at all", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", nlohmann::json{{"type", "minecraft:water"},
                                                          {"offset", -20},
                                                          {"surface_depth_multiplier", 0},
                                                          {"add_stone_depth", false}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    // No fluid: true everywhere, at any y, whatever the offset. `sea_level`
    // alone does not make a water height — only real fluid blocks do.
    CHECK(executor.apply(at(0, -60, 0)) != nullptr);
    CHECK(executor.apply(at(0, 300, 0)) != nullptr);

    // With a fluid band topping out at y = 1, the latched height is 2 and the
    // boundary is 2 + (-20) = -18.
    Context wet = at(0, -18, 0);
    wet.waterHeight = 2;
    CHECK(executor.apply(wet) != nullptr);
    Context below = at(0, -19, 0);
    below.waterHeight = 2;
    CHECK(executor.apply(below) == nullptr);
}

TEST_CASE("steep is asymmetric, and that is not a bug to tidy away", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph = resolve(nlohmann::json{{"type", "minecraft:condition"},
                                                   {"if_true", {{"type", "minecraft:steep"}}},
                                                   {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    const auto fires = [&](std::int32_t w, std::int32_t e, std::int32_t n, std::int32_t s) {
        Context ctx = at(8, 100, 8);
        ctx.heightWest = w;
        ctx.heightEast = e;
        ctx.heightNorth = n;
        ctx.heightSouth = s;
        return executor.apply(ctx) != nullptr;
    };

    // West minus east, south minus north. The opposite signs must NOT fire —
    // 17375 columns of measured evidence say so, and an abs() would break them.
    CHECK(fires(10, 6, 0, 0));
    CHECK_FALSE(fires(6, 10, 0, 0));
    CHECK(fires(0, 0, 6, 10));
    CHECK_FALSE(fires(0, 0, 10, 6));

    // The threshold is >= 4 on integers: three is not steep, four is.
    CHECK_FALSE(fires(9, 6, 0, 0));
    CHECK(fires(10, 6, 0, 0));

    // OR, not AND.
    CHECK(fires(10, 6, 10, 6));
}

TEST_CASE("the steep neighbours are clamped inside the block's own chunk", "[surface]") {
    // At a chunk edge the clamp repeats the edge column rather than reading
    // the neighbouring chunk — which is what keeps steep free of a cross-chunk
    // dependency in the hot path.
    std::vector<std::pair<std::int32_t, std::int32_t>> asked;
    Context edge = at(0, 100, 5);
    stratum::surface::fillSteepNeighbours(edge, [&](std::int32_t x, std::int32_t z) {
        asked.emplace_back(x, z);
        return 0;
    });
    for (const auto& [x, z] : asked) {
        CHECK(x >= 0);
        CHECK(x <= 15);
    }

    // Deep inside a chunk it reads the true neighbours.
    asked.clear();
    Context middle = at(8, 100, 8);
    stratum::surface::fillSteepNeighbours(middle, [&](std::int32_t x, std::int32_t z) {
        asked.emplace_back(x, z);
        return 0;
    });
    CHECK(asked[0].first == 7);
    CHECK(asked[1].first == 9);
}
