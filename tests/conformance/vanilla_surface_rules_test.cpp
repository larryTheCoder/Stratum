// Stratum — vanilla's own surface rules, resolved.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Every one of the seven dimensions vanilla ships, loaded whole. This is the
// same claim the density graph makes about density functions: the structure
// of the data is understood well enough to read all of it, and what cannot be
// EXECUTED is named rather than skipped (SPEC §8).
//
// The counts are pinned rather than thresholded. The overworld's surface rule
// is 287 rules over 141 conditions — a tree an order of magnitude larger than
// anything else in this project — and a change in it means vanilla's data
// moved or this build's reading of it did. Either is worth a person looking.
#include <stratum/data/pack.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>

namespace {

[[nodiscard]] std::filesystem::path worldgenTree() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "worldgen";
}

struct Shape {
    std::size_t rules = 0;
    std::size_t conditions = 0;
    std::size_t noises = 0;
};

} // namespace

TEST_CASE("every dimension's surface rules resolve, and say what cannot run",
          "[conformance][surface]") {
    const std::filesystem::path tree = worldgenTree();
    if (!std::filesystem::is_directory(tree)) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    REQUIRE(loaded.settings.size() == 7U);

    const std::map<std::string, Shape> expected{
        {"minecraft:amplified", {.rules = 287, .conditions = 141, .noises = 7}},
        {"minecraft:caves", {.rules = 288, .conditions = 142, .noises = 7}},
        // The End's is a single `block` rule: no conditions, no noises, and
        // the only one of the seven this build could run today.
        {"minecraft:end", {.rules = 1, .conditions = 0, .noises = 0}},
        {"minecraft:floating_islands", {.rules = 284, .conditions = 139, .noises = 7}},
        {"minecraft:large_biomes", {.rules = 287, .conditions = 141, .noises = 7}},
        {"minecraft:nether", {.rules = 75, .conditions = 51, .noises = 6}},
        {"minecraft:overworld", {.rules = 287, .conditions = 141, .noises = 7}},
    };

    for (const auto& [id, settings] : loaded.settings) {
        CAPTURE(id.toString());
        const auto graph = stratum::surface::RuleGraph::resolve(settings.surfaceRule, id);

        const auto found = expected.find(id.toString());
        REQUIRE(found != expected.end());
        CHECK(graph.ruleCount() == found->second.rules);
        CHECK(graph.conditionCount() == found->second.conditions);
        CHECK(graph.referencedNoises().size() == found->second.noises);

        if (id.toString() == "minecraft:end") {
            // Nothing in it is refused, which makes it the one dimension whose
            // surface rules this build could execute the moment there is an
            // executor to run them.
            CHECK(graph.unrunnable().empty());
        } else {
            CHECK_FALSE(graph.unrunnable().empty());
        }
    }
}

TEST_CASE("the overworld names exactly the constructs SPEC accounts for",
          "[conformance][surface]") {
    const std::filesystem::path tree = worldgenTree();
    if (!std::filesystem::is_directory(tree)) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto id = stratum::data::ResourceLocation::parse("minecraft:overworld");
    const auto graph = stratum::surface::RuleGraph::resolve(loaded.settings.at(id).surfaceRule, id);

    // Nine of the fifteen types the overworld uses cannot be run yet, and the
    // list is asserted whole rather than counted: when one of them is settled
    // it leaves this list, and that should be a visible, deliberate edit.
    //
    // `minecraft:vertical_gradient` is the first to leave. Its random source
    // was recovered and checked against the server on 27 million blocks
    // (SPEC §11), which is what took this list from ten to nine.
    const std::vector<std::string> expected{
        "minecraft:bandlands",       "minecraft:biome", "minecraft:hole",
        "minecraft:noise_threshold", "minecraft:steep", "minecraft:stone_depth",
        "minecraft:temperature",     "minecraft:water", "minecraft:y_above",
    };
    CHECK(graph.unrunnable() == expected);

    // The six that are not in that list are the ones that need nothing this
    // build lacks: the three structural rules, plus `not`,
    // `above_preliminary_surface` and now `vertical_gradient`.
    CHECK(graph.unrunnable().size() == 9U);
}
