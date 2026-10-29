// Stratum — the biome the surface pass hands its rules, quart by quart.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// terrain::ChunkFiller's surface pass resolves one biome per quart of the
// chunk, the first time a block of that quart asks, and keeps it for every
// later block of the quart. The cases here hold that to a reference computed
// beside the engine — biome::Source::at over a separate Interpreter, at each
// block's own quart corner — on a dimension whose climate varies in x, y AND
// z at quart scale, so that a quart answered from another quart's slot is a
// different block.
#include "support/temp_path.hpp"

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/source.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using stratum::data::ResourceLocation;

class BiomeTree {
public:
    BiomeTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        std::filesystem::create_directories(path_ / "noise");
    }

    BiomeTree(const BiomeTree&) = delete;
    BiomeTree& operator=(const BiomeTree&) = delete;
    BiomeTree(BiomeTree&&) = delete;
    BiomeTree& operator=(BiomeTree&&) = delete;

    ~BiomeTree() {
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
        return stratum::test::tempName("stratum-surface-pass-biome-test");
    }

    std::filesystem::path path_;
};

/// Solid stone from y = -64 to -1, water to sea level 8, air above; a
/// temperature that climbs with y and wanders with a noise on 8- and 4-block
/// octaves in x and z, so the quarts of one chunk disagree in every
/// direction; every other climate entry zero.
[[nodiscard]] nlohmann::json settingsJson() {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(
            static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
    }
    router["final_density"] = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                             {"from_y", -1},
                                             {"to_y", 1},
                                             {"from_value", 1.0},
                                             {"to_value", -1.0}};
    router["temperature"] = nlohmann::json{{"type", "minecraft:add"},
                                           {"argument1",
                                            {{"type", "minecraft:y_clamped_gradient"},
                                             {"from_y", -64},
                                             {"to_y", 0},
                                             {"from_value", -0.8},
                                             {"to_value", 0.8}}},
                                           {"argument2",
                                            {{"type", "minecraft:mul"},
                                             {"argument1", 0.45},
                                             {"argument2",
                                              {{"type", "minecraft:noise"},
                                               {"noise", "minecraft:climate_field"},
                                               {"xz_scale", 1.0},
                                               {"y_scale", 0.0}}}}}};
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 8},
        {"disable_mob_generation", false},
        {"aquifers_enabled", false},
        {"ore_veins_enabled", false},
        {"legacy_random_source", false},
        {"noise", {{"min_y", -64}, {"height", 128}, {"size_horizontal", 1}, {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

/// Five biomes, one per band of temperature, every other axis open.
[[nodiscard]] stratum::biome::ParameterList bandedBiomes() {
    const nlohmann::json open = nlohmann::json::array({-1.0, 1.0});
    const std::array<std::pair<const char*, std::array<double, 2>>, 5> bands{{
        {"stratum:a", {-2.0, -0.45}},
        {"stratum:b", {-0.45, -0.15}},
        {"stratum:c", {-0.15, 0.15}},
        {"stratum:d", {0.15, 0.45}},
        {"stratum:e", {0.45, 2.0}},
    }};
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [name, band] : bands) {
        entries.push_back({{"biome", name},
                           {"parameters",
                            {{"temperature", nlohmann::json::array({band[0], band[1]})},
                             {"humidity", open},
                             {"continentalness", open},
                             {"erosion", open},
                             {"depth", open},
                             {"weirdness", open},
                             {"offset", 0.0}}}});
    }
    return stratum::biome::ParameterList::fromJson(nlohmann::json{{"biomes", entries}},
                                                   ResourceLocation::parse("minecraft:test"));
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

[[nodiscard]] nlohmann::json whenBiome(const std::vector<std::string>& biomes,
                                       const std::string& name) {
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", {{"type", "minecraft:biome"}, {"biome_is", biomes}}},
                          {"then_run", block(name)}};
}

} // namespace

TEST_CASE("the surface pass reads each quart's own biome, in every direction",
          "[terrain][filler][surface]") {
    const BiomeTree tree;
    tree.write("noise_settings/test.json", settingsJson().dump());
    tree.write("noise/climate_field.json", R"({"firstOctave": -3, "amplitudes": [1.0, 1.0]})");
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(tree.pack());
    const auto& settings = loaded.settings.at(ResourceLocation::parse("minecraft:test"));
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    const stratum::biome::ParameterList biomes = bandedBiomes();

    // One block per biome, a list of two among them, and `stratum:e` placing
    // nothing: every default block then names the biome it was answered for,
    // or that it was answered for `stratum:e`.
    const stratum::surface::RuleGraph surface = stratum::surface::RuleGraph::resolve(
        nlohmann::json{{"type", "minecraft:sequence"},
                       {"sequence",
                        {whenBiome({"stratum:a"}, "minecraft:dirt"),
                         whenBiome({"stratum:b", "stratum:c"}, "minecraft:sand"),
                         whenBiome({"stratum:c"}, "minecraft:gravel"),
                         whenBiome({"stratum:d"}, "minecraft:clay")}}},
        ResourceLocation::parse("minecraft:test"));
    const auto filler =
        stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings, &surface, &biomes);
    REQUIRE(filler.surfaceRulesBlockedBy().empty());
    REQUIRE(filler.runsSurfaceRules());

    const std::map<std::string, std::string> placedFor{{"stratum:a", "minecraft:dirt"},
                                                       {"stratum:b", "minecraft:sand"},
                                                       {"stratum:c", "minecraft:sand"},
                                                       {"stratum:d", "minecraft:clay"},
                                                       {"stratum:e", "minecraft:stone"}};

    const stratum::density::Interpreter reference(loaded.graph, noises);
    stratum::density::Interpreter::CornerCache cache(reference.cacheSize());
    const stratum::biome::Source source = stratum::biome::Source::multiNoise(biomes);
    const auto quart = [](std::int32_t value) {
        return stratum::javamath::floorDiv(value, std::int32_t{4}) * 4;
    };

    stratum::terrain::ChunkBuffer buffer(settings.geometry);
    std::set<std::string> seen;
    std::size_t splitLayers = 0;
    std::size_t checked = 0;
    for (const auto& [chunkX, chunkZ] :
         std::array<std::pair<std::int32_t, std::int32_t>, 3>{{{0, 0}, {-3, 2}, {5, -7}}}) {
        filler.fill(chunkX, chunkZ, buffer);
        for (std::int32_t y = -64; y < 0; ++y) {
            std::set<std::string> layer;
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    const ResourceLocation& biome = source.at(
                        reference, settings.router,
                        stratum::density::Point{.x = quart(x), .y = quart(y), .z = quart(z)},
                        cache);
                    CAPTURE(chunkX, chunkZ, localX, y, localZ, biome.toString());
                    REQUIRE(buffer.at(localX, y, localZ).name.toString() ==
                            placedFor.at(biome.toString()));
                    seen.insert(biome.toString());
                    layer.insert(biome.toString());
                    ++checked;
                }
            }
            splitLayers += layer.size() > 1U ? 1U : 0U;
        }
    }
    CHECK(checked == 3U * 64U * 256U);
    // Every biome somewhere, and quarts of one layer disagreeing — which is
    // what makes a quart read from its neighbour's slot a different block.
    CHECK(seen.size() == 5U);
    CHECK(splitLayers > 60U);
}
