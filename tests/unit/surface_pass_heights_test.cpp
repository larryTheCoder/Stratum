// Stratum — the surface pass's column heights and runs, through the filler.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// terrain::ChunkFiller's surface pass finds each column's topmost non-air
// block a layer at a time — for `steep`'s neighbours, and as the point above
// which SurfaceColumn's read need not look — and counts each column's runs
// off its buffer. This holds both to a reference read off the SAME chunk
// filled without surface rules, on terrain whose height and water vary from
// column to column, so that a height or a run read anywhere else is a
// different block.
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using stratum::data::ResourceLocation;

class HeightsTree {
public:
    HeightsTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        std::filesystem::create_directories(path_ / "noise");
    }

    HeightsTree(const HeightsTree&) = delete;
    HeightsTree& operator=(const HeightsTree&) = delete;
    HeightsTree(HeightsTree&&) = delete;
    HeightsTree& operator=(HeightsTree&&) = delete;

    ~HeightsTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void write(const std::filesystem::path& relative, std::string_view text) const {
        std::ofstream out(path_ / relative);
        out << text;
    }

    [[nodiscard]] stratum::data::Pack pack() const { return stratum::data::Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-surface-pass-heights-test");
    }

    std::filesystem::path path_;
};

/// Rolling terrain: a y gradient around 0 lifted and sunk by a noise on 8-
/// and 4-block octaves, with overhangs where a second noise cuts into it, and
/// water up to sea level 2 over whatever lies lower.
[[nodiscard]] nlohmann::json settingsJson() {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(
            static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
    }
    const nlohmann::json field{{"type", "minecraft:noise"},
                               {"noise", "minecraft:terrain_field"},
                               {"xz_scale", 1.0},
                               {"y_scale", 0.0}};
    const nlohmann::json caves{{"type", "minecraft:noise"},
                               {"noise", "minecraft:cave_field"},
                               {"xz_scale", 1.0},
                               {"y_scale", 1.0}};
    router["final_density"] = nlohmann::json{
        {"type", "minecraft:add"},
        {"argument1",
         {{"type", "minecraft:add"},
          {"argument1",
           {{"type", "minecraft:y_clamped_gradient"},
            {"from_y", -24},
            {"to_y", 24},
            {"from_value", 1.0},
            {"to_value", -1.0}}},
          {"argument2", {{"type", "minecraft:mul"}, {"argument1", 0.5}, {"argument2", field}}}}},
        {"argument2", {{"type", "minecraft:mul"}, {"argument1", -0.25}, {"argument2", caves}}}};
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 2},
        {"disable_mob_generation", false},
        {"aquifers_enabled", false},
        {"ore_veins_enabled", false},
        {"legacy_random_source", false},
        {"noise", {{"min_y", -32}, {"height", 96}, {"size_horizontal", 1}, {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

} // namespace

TEST_CASE("the surface pass reads each column's height and runs off the chunk it filled",
          "[terrain][filler][surface]") {
    const HeightsTree tree;
    tree.write("noise_settings/test.json", settingsJson().dump());
    tree.write("noise/terrain_field.json", R"({"firstOctave": -3, "amplitudes": [1.0, 1.0]})");
    tree.write("noise/cave_field.json", R"({"firstOctave": -2, "amplitudes": [1.0]})");
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(tree.pack());
    const auto& settings = loaded.settings.at(ResourceLocation::parse("minecraft:test"));
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);

    // steep first, then the top of every run of stone, then a ceiling's
    // bottom block: each block placed says which reading it came from.
    const auto when = [](const nlohmann::json& condition, const std::string& name) {
        return nlohmann::json{
            {"type", "minecraft:condition"}, {"if_true", condition}, {"then_run", block(name)}};
    };
    const nlohmann::json stoneDepth = [](const char* surfaceType) {
        return nlohmann::json{{"type", "minecraft:stone_depth"},
                              {"offset", 0},
                              {"surface_type", surfaceType},
                              {"add_surface_depth", false},
                              {"secondary_depth_range", 0}};
    }("floor");
    const nlohmann::json ceiling{{"type", "minecraft:stone_depth"},
                                 {"offset", 0},
                                 {"surface_type", "ceiling"},
                                 {"add_surface_depth", false},
                                 {"secondary_depth_range", 0}};
    const stratum::surface::RuleGraph surface = stratum::surface::RuleGraph::resolve(
        nlohmann::json{
            {"type", "minecraft:sequence"},
            {"sequence",
             {when({{"type", "minecraft:steep"}}, "minecraft:magma_block"),
              when(stoneDepth, "minecraft:grass_block"), when(ceiling, "minecraft:sandstone")}}},
        ResourceLocation::parse("minecraft:test"));
    const auto bare = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings);
    const auto surfaced =
        stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings, &surface);
    REQUIRE(surfaced.runsSurfaceRules());

    const std::int32_t minY = settings.geometry.minY;
    const std::int32_t topY = minY + settings.geometry.height;
    stratum::terrain::ChunkBuffer first(settings.geometry);
    stratum::terrain::ChunkBuffer second(settings.geometry);
    std::size_t steep = 0;
    std::size_t tops = 0;
    std::size_t ceilings = 0;
    std::size_t pooled = 0;
    for (const auto& [chunkX, chunkZ] :
         std::array<std::pair<std::int32_t, std::int32_t>, 3>{{{0, 0}, {-2, 5}, {7, -3}}}) {
        bare.fill(chunkX, chunkZ, first);
        surfaced.fill(chunkX, chunkZ, second);
        const auto name = [&](int x, std::int32_t y, int z) {
            return first.at(x, y, z).name.toString();
        };
        // WORLD_SURFACE off the bare chunk: the highest y that is not air.
        std::array<std::int32_t, 256> height{};
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                std::int32_t y = topY - 1;
                while (y >= minY && name(x, y, z) == "minecraft:air") {
                    --y;
                }
                height[static_cast<std::size_t>((z * 16) + x)] = y;
            }
        }
        const auto heightAt = [&](int x, int z) {
            return height[static_cast<std::size_t>((std::clamp(z, 0, 15) * 16) +
                                                   std::clamp(x, 0, 15))];
        };
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const bool isSteep = heightAt(x - 1, z) - heightAt(x + 1, z) >= 4 ||
                                     heightAt(x, z + 1) - heightAt(x, z - 1) >= 4;
                // The top run descending: air resets it, fluid holds it.
                std::int32_t run = 0;
                std::int32_t floorRun = 0;
                std::array<std::int32_t, 96> above{};
                for (std::int32_t y = topY - 1; y >= minY; --y) {
                    const std::string here = name(x, y, z);
                    if (here == "minecraft:air") {
                        run = 0;
                    } else if (here == "minecraft:stone") {
                        ++run;
                    }
                    above[static_cast<std::size_t>(y - minY)] = run;
                }
                for (std::int32_t y = minY; y < topY; ++y) {
                    const std::string here = name(x, y, z);
                    floorRun = here == "minecraft:stone" ? floorRun + 1 : 0;
                    std::string expected = here;
                    if (here == "minecraft:stone" && y <= heightAt(x, z)) {
                        if (isSteep) {
                            expected = "minecraft:magma_block";
                        } else if (above[static_cast<std::size_t>(y - minY)] == 1) {
                            expected = "minecraft:grass_block";
                        } else if (floorRun == 1) {
                            expected = "minecraft:sandstone";
                        }
                    }
                    CAPTURE(chunkX, chunkZ, x, y, z);
                    REQUIRE(second.at(x, y, z).name.toString() == expected);
                    steep += expected == "minecraft:magma_block" ? 1U : 0U;
                    tops += expected == "minecraft:grass_block" ? 1U : 0U;
                    ceilings += expected == "minecraft:sandstone" ? 1U : 0U;
                    pooled += here == "minecraft:water" ? 1U : 0U;
                }
            }
        }
    }
    // Every reading decided blocks, and the terrain held water.
    CHECK(steep > 100U);
    CHECK(tops > 100U);
    CHECK(ceilings > 10U);
    CHECK(pooled > 100U);
}
