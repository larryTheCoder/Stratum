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
        // The End's is a single `block` rule: no conditions, no noises.
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

        // Every dimension is fully runnable now, and that is itself the
        // progress report — asserted per dimension rather than counted, so
        // a future regression names which one broke. The End was first and
        // was alone for a long time; the Nether joined it once
        // vertical_gradient and the surface depth landed; the remaining
        // five joined together once `bandlands` closed
        // (spec/bandlands-spec.md, SPEC §11) — it was the last construct
        // any of the seven trees named.
        CHECK(graph.unrunnable().empty());
    }
}

TEST_CASE("the overworld names nothing left unrunnable", "[conformance][surface]") {
    const std::filesystem::path tree = worldgenTree();
    if (!std::filesystem::is_directory(tree)) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto id = stratum::data::ResourceLocation::parse("minecraft:overworld");
    const auto graph = stratum::surface::RuleGraph::resolve(loaded.settings.at(id).surfaceRule, id);

    // RESOLVED. This list — asserted whole rather than counted throughout
    // its life, so that closing an entry was always a visible, deliberate
    // edit — has gone ten, nine, three, two, one, zero.
    //
    // vertical_gradient left when its random source was recovered; surface
    // depth then unblocked hole, steep, stone_depth, water, y_above and
    // noise_threshold together; `biome` left because it needed the biome
    // plumbed through rather than derived; `temperature` left once a height
    // sweep showed it compares a height-adjusted value rather than the flat
    // threshold recorded here for two milestones; and `bandlands`, the
    // overworld's own last remaining construct, closed once its colour
    // table's construction was derived clean-room and confirmed exactly
    // against three world seeds and 532224 real blocks
    // (spec/bandlands-spec.md, SPEC §11).
    //
    // This file only checks that the SCHEMA has nothing left unrunnable in
    // it — whether `ChunkFiller` can actually RUN a tree is a distinct
    // question (a missing INPUT, not an unrunnable CONSTRUCT) tracked in
    // terrain/filler.hpp's own doc. As of `biome::TemperatureTable`
    // (SPEC §11), the overworld's own tree runs end to end against real
    // blocks too — see golden_fill_test.cpp — so both questions this file
    // and that one ask now answer the same way for every dimension with
    // fixtures.
    CHECK(graph.unrunnable().empty());
}
