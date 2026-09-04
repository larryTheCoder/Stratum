// Stratum — resolving surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::surface::ConditionType;
using stratum::surface::RuleError;
using stratum::surface::RuleGraph;
using stratum::surface::RuleType;
using stratum::surface::VerticalAnchor;

namespace {

[[nodiscard]] RuleGraph resolve(const nlohmann::json& json) {
    return RuleGraph::resolve(json, ResourceLocation::parse("minecraft:test"));
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

} // namespace

TEST_CASE("a sequence of conditions resolves to a tree", "[surface]") {
    const RuleGraph graph = resolve(nlohmann::json{
        {"type", "minecraft:sequence"},
        {"sequence",
         {nlohmann::json{{"type", "minecraft:condition"},
                         {"if_true", {{"type", "minecraft:above_preliminary_surface"}}},
                         {"then_run", block("minecraft:grass_block")}},
          block("minecraft:stone")}}});

    REQUIRE(graph.rule(graph.root()).type == RuleType::Sequence);
    const auto& sequence = graph.rule(graph.root()).sequence;
    REQUIRE(sequence.size() == 2U);
    CHECK(graph.rule(sequence[0]).type == RuleType::Condition);
    CHECK(graph.rule(sequence[1]).type == RuleType::Block);
    CHECK(graph.rule(sequence[1]).block.name == ResourceLocation::parse("minecraft:stone"));
    CHECK(graph.condition(graph.rule(sequence[0]).condition).type ==
          ConditionType::AbovePreliminarySurface);
}

TEST_CASE("all three vertical anchor spellings resolve against a dimension", "[surface]") {
    // Vanilla writes all three: 72 absolute, 10 above_bottom, 5 below_top
    // across its seven settings, so all three have to work.
    const stratum::settings::NoiseGeometry geometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
    CHECK(VerticalAnchor{.kind = VerticalAnchor::Kind::Absolute, .value = 8}.resolve(geometry) ==
          8);
    CHECK(VerticalAnchor{.kind = VerticalAnchor::Kind::AboveBottom, .value = 5}.resolve(geometry) ==
          -59);
    // The top block is minY + height - 1, so below_top of nought is that block.
    CHECK(VerticalAnchor{.kind = VerticalAnchor::Kind::BelowTop, .value = 0}.resolve(geometry) ==
          319);
    CHECK(VerticalAnchor{.kind = VerticalAnchor::Kind::BelowTop, .value = 4}.resolve(geometry) ==
          315);
}

TEST_CASE("a vertical gradient keeps its name and both anchors", "[surface]") {
    const RuleGraph graph = resolve(nlohmann::json{{"type", "minecraft:condition"},
                                                   {"if_true",
                                                    {{"type", "minecraft:vertical_gradient"},
                                                     {"random_name", "minecraft:deepslate"},
                                                     {"true_at_and_below", {{"absolute", 0}}},
                                                     {"false_at_and_above", {{"absolute", 8}}}}},
                                                   {"then_run", block("minecraft:deepslate")}});
    const auto& condition = graph.condition(graph.rule(graph.root()).condition);
    CHECK(condition.type == ConditionType::VerticalGradient);
    CHECK(condition.randomName == "minecraft:deepslate");
    CHECK(condition.trueAtAndBelow ==
          VerticalAnchor{.kind = VerticalAnchor::Kind::Absolute, .value = 0});
    CHECK(condition.falseAtAndAbove ==
          VerticalAnchor{.kind = VerticalAnchor::Kind::Absolute, .value = 8});
}

TEST_CASE("noises named by conditions are collected once each", "[surface]") {
    const auto threshold = [](const std::string& noise) {
        return nlohmann::json{{"type", "minecraft:condition"},
                              {"if_true",
                               {{"type", "minecraft:noise_threshold"},
                                {"noise", noise},
                                {"min_threshold", -0.5},
                                {"max_threshold", 0.5}}},
                              {"then_run", block("minecraft:stone")}};
    };
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:sequence"},
                               {"sequence",
                                {threshold("minecraft:surface"), threshold("minecraft:surface"),
                                 threshold("minecraft:powder_snow")}}});
    const auto noises = graph.referencedNoises();
    REQUIRE(noises.size() == 2U);
    CHECK(noises[0] == ResourceLocation::parse("minecraft:powder_snow"));
    CHECK(noises[1] == ResourceLocation::parse("minecraft:surface"));
}

TEST_CASE("what cannot be run is named, and what can be is not", "[surface]") {
    // above_preliminary_surface runs: find_top_surface landed, so the
    // preliminary surface level is a thing this build computes. `not` runs
    // because it is only a negation. Everything else in the tree is refused
    // with a reason (SPEC §11).
    const RuleGraph runnable =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true",
                                {{"type", "minecraft:not"},
                                 {"invert", {{"type", "minecraft:above_preliminary_surface"}}}}},
                               {"then_run", block("minecraft:stone")}});
    CHECK(runnable.unrunnable().empty());

    const RuleGraph refused = resolve(nlohmann::json{{"type", "minecraft:condition"},
                                                     {"if_true", {{"type", "minecraft:steep"}}},
                                                     {"then_run", block("minecraft:stone")}});
    REQUIRE(refused.unrunnable().size() == 1U);
    CHECK(refused.unrunnable()[0] == "minecraft:steep");

    // And the reason says what is missing, not merely that something is.
    const auto reason = RuleGraph::unrunnableReason(ConditionType::Steep);
    REQUIRE(reason.has_value());
    CHECK_THAT(std::string(*reason),
               ContainsSubstring("neighbouring columns") && ContainsSubstring("SPEC"));

    // vertical_gradient is no longer among them: its random source was
    // recovered and checked against the server on 27 million blocks.
    CHECK_FALSE(RuleGraph::unrunnableReason(ConditionType::VerticalGradient).has_value());
}

TEST_CASE("a malformed surface rule is refused by name", "[surface]") {
    CHECK_THROWS_WITH(resolve(nlohmann::json{{"type", "minecraft:no_such_rule"}}),
                      ContainsSubstring("minecraft:no_such_rule"));
    CHECK_THROWS_WITH(resolve(nlohmann::json{{"type", "minecraft:condition"},
                                             {"if_true", {{"type", "minecraft:no_such_condition"}}},
                                             {"then_run", block("minecraft:stone")}}),
                      ContainsSubstring("minecraft:no_such_condition"));
    // A missing field is named, with the type that wanted it.
    CHECK_THROWS_WITH(resolve(nlohmann::json{{"type", "minecraft:sequence"}}),
                      ContainsSubstring("sequence"));
    // surface_type is a closed set of two.
    CHECK_THROWS_WITH(resolve(nlohmann::json{{"type", "minecraft:condition"},
                                             {"if_true",
                                              {{"type", "minecraft:stone_depth"},
                                               {"offset", 0},
                                               {"add_surface_depth", false},
                                               {"secondary_depth_range", 0},
                                               {"surface_type", "sideways"}}},
                                             {"then_run", block("minecraft:stone")}}),
                      ContainsSubstring("sideways") && ContainsSubstring("floor"));
    // So is the set of vertical anchors.
    CHECK_THROWS_WITH(resolve(nlohmann::json{{"type", "minecraft:condition"},
                                             {"if_true",
                                              {{"type", "minecraft:y_above"},
                                               {"anchor", {{"sideways", 4}}},
                                               {"surface_depth_multiplier", 0},
                                               {"add_stone_depth", false}}},
                                             {"then_run", block("minecraft:stone")}}),
                      ContainsSubstring("sideways") && ContainsSubstring("above_bottom"));
    CHECK_THROWS_AS(resolve(nlohmann::json{{"type", "minecraft:block"}}), RuleError);
}

TEST_CASE("a vertical gradient is certain below, impossible above, and drawn between",
          "[surface]") {
    const auto source = stratum::rng::positionalSourceFor(42, "minecraft:deepslate");
    const auto fires = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return stratum::surface::verticalGradientFires(source, x, y, z, -100, 124);
    };

    // The ends need no draw at all.
    CHECK(fires(0, -100, 0));
    CHECK(fires(0, -400, 0));
    CHECK_FALSE(fires(0, 124, 0));
    CHECK_FALSE(fires(0, 400, 0));

    // Between them the rate has to track the probability. At y = 12 that is
    // (124 - 12) / 224 = 0.5 exactly, so a large sample should sit near half.
    int fired = 0;
    int total = 0;
    for (std::int32_t x = 0; x < 64; ++x)
        for (std::int32_t z = 0; z < 64; ++z) {
            ++total;
            fired += static_cast<int>(fires(x, 12, z));
        }
    const double rate = static_cast<double>(fired) / static_cast<double>(total);
    CHECK(rate > 0.46);
    CHECK(rate < 0.54);

    // The name is a salt, so an unrelated one gives an unrelated field.
    const auto other = stratum::rng::positionalSourceFor(42, "stratum:some_other_name");
    int differing = 0;
    for (std::int32_t x = 0; x < 64; ++x)
        for (std::int32_t z = 0; z < 64; ++z)
            differing += static_cast<int>(stratum::surface::verticalGradientFires(
                                              other, x, 12, z, -100, 124) != fires(x, 12, z));
    CHECK(differing > total / 4);

    // But NOT every pair of names is unrelated, and this is measured rather
    // than assumed: over one probe world the server's own deepslate and
    // bedrock_floor fields differ on 24.6% of blocks where two independent
    // fields differ on 50.7%. Whatever brings those two salts close together,
    // this build inherits it — both are reproduced exactly — so a test that
    // demanded independence between them would be asserting a falsehood.
    const auto bedrock = stratum::rng::positionalSourceFor(42, "minecraft:bedrock_floor");
    int alsoDiffering = 0;
    for (std::int32_t x = 0; x < 64; ++x)
        for (std::int32_t z = 0; z < 64; ++z)
            alsoDiffering += static_cast<int>(stratum::surface::verticalGradientFires(
                                                  bedrock, x, 12, z, -100, 124) != fires(x, 12, z));
    CHECK(alsoDiffering < differing);
}
