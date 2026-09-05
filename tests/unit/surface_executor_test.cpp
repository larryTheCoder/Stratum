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
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:sequence"},
                               {"sequence",
                                {nlohmann::json{{"type", "minecraft:condition"},
                                                {"if_true", gradient("minecraft:deepslate", -8, 8)},
                                                {"then_run", block("minecraft:deepslate")}},
                                 nlohmann::json{{"type", "minecraft:condition"},
                                                {"if_true", {{"type", "minecraft:steep"}}},
                                                {"then_run", block("minecraft:stone")}}}}});

    // The runnable half is not a licence to run the tree: one unrunnable
    // branch refuses all of it, and the message says which and why.
    CHECK_THROWS_WITH(Executor::compile(graph, kSeed, geometry),
                      ContainsSubstring("minecraft:steep") && ContainsSubstring("refused"));
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
