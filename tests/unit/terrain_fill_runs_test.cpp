// Stratum — the fill's runs: what a run of the block kernel alone decides.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ChunkFiller::fill runs final_density (and vein_toggle, on the solid lanes
// inside the vein range) over a cell's blocks of one y at once, and places
// from the run's lanes every block that needs nothing more read: solid
// ground where no vein can be, and ground that is not solid where the
// aquifer is not asked — a whole run at once where every lane holds one
// state. The rest go block by block. This holds the fill to the rule it
// implements, written out block by block here from the interpreter's walk
// and the vein module: over runs that are all one state and runs that mix
// solid ground, open fluid and vein candidates, in the lava sea, the water
// and the air, and to the palette's order, which is the order blocks are
// first placed.
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/settings/noise_settings.hpp>
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
#include <system_error>
#include <utility>
#include <vector>

using stratum::data::ResourceLocation;
using stratum::settings::BlockState;
using stratum::settings::RouterEntry;
using stratum::terrain::ChunkBuffer;
using stratum::terrain::ChunkFiller;

namespace {

class RunsTree {
public:
    RunsTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        // A worldgen tree is one with density_function/, here empty.
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    RunsTree(const RunsTree&) = delete;
    RunsTree& operator=(const RunsTree&) = delete;
    RunsTree(RunsTree&&) = delete;
    RunsTree& operator=(RunsTree&&) = delete;

    ~RunsTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void defineNoise(std::string_view name, std::string_view json) const {
        std::ofstream out(path_ / "noise" / (std::string(name) + ".json"));
        out << json;
    }

    void defineSettings(std::string_view name, const nlohmann::json& json) const {
        std::ofstream out(path_ / "noise_settings" / (std::string(name) + ".json"));
        out << json.dump();
    }

    [[nodiscard]] stratum::data::Pack pack() const { return stratum::data::Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-fill-runs-test");
    }

    std::filesystem::path path_;
};

[[nodiscard]] nlohmann::json noiseNode(std::string_view noise, double scale) {
    return nlohmann::json{{"type", "minecraft:noise"},
                          {"noise", std::string("minecraft:") + std::string(noise)},
                          {"xz_scale", scale},
                          {"y_scale", scale}};
}

[[nodiscard]] nlohmann::json mul(double factor, const nlohmann::json& argument) {
    return nlohmann::json{
        {"type", "minecraft:mul"}, {"argument1", factor}, {"argument2", argument}};
}

/// 128 blocks from y = -64, cells 4 by 4, veins on and aquifers off. The
/// ground is a plane at y = 0 roughened by a noise strong enough to open
/// caves at the floor (lava under -54, water under the sea at 8) and to
/// carry the surface across cells; vein_toggle is a noise wide enough to
/// clear the richness gate on some lanes of a run and not on others.
[[nodiscard]] nlohmann::json runsSettings() {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }
    const nlohmann::json plane{{"type", "minecraft:y_clamped_gradient"},
                               {"from_y", -64},
                               {"to_y", 64},
                               {"from_value", 0.6},
                               {"to_value", -0.6}};
    router["final_density"] = nlohmann::json{{"type", "minecraft:interpolated"},
                                             {"argument",
                                              {{"type", "minecraft:add"},
                                               {"argument1", plane},
                                               {"argument2", mul(1.2, noiseNode("field", 1.0))}}}};
    router["vein_toggle"] = mul(1.5, noiseNode("toggle", 1.0));
    router["vein_ridged"] = noiseNode("ridged", 1.0);
    router["vein_gap"] = noiseNode("gap", 1.0);
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 8},
        {"disable_mob_generation", false},
        {"aquifers_enabled", false},
        {"ore_veins_enabled", true},
        {"legacy_random_source", false},
        {"noise", {{"min_y", -64}, {"height", 128}, {"size_horizontal", 1}, {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

[[nodiscard]] BlockState named(std::string_view name) {
    return BlockState{.name = ResourceLocation::parse(name), .properties = {}};
}

} // namespace

TEST_CASE("the fill places from a run's lanes exactly what each block's own reads place",
          "[terrain][filler][ore]") {
    const RunsTree tree;
    tree.defineNoise("field", R"({"firstOctave": -4, "amplitudes": [1.0, 0.5]})");
    tree.defineNoise("toggle", R"({"firstOctave": -3, "amplitudes": [1.0]})");
    tree.defineNoise("ridged", R"({"firstOctave": -2, "amplitudes": [1.0]})");
    tree.defineNoise("gap", R"({"firstOctave": -2, "amplitudes": [1.0]})");
    tree.defineSettings("runs", runsSettings());

    const stratum::data::Pack pack = tree.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    const auto& settings = loaded.settings.at(ResourceLocation::parse("minecraft:runs"));
    const auto& geometry = settings.geometry;
    const ChunkFiller filler = ChunkFiller::compile(loaded.graph, noises, settings);

    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = geometry.cellWidth(),
                                       .height = geometry.cellHeight()});
    const stratum::ore::VeinSource veins(noises.worldSeed(), noises.source());
    const auto entry = [&settings](const RouterEntry e) { return settings.router.at(e); };

    // The six vein blocks, by metal and then filler, ore, raw block.
    const std::array<BlockState, 6> veinBlocks{named("minecraft:granite"),
                                               named("minecraft:copper_ore"),
                                               named("minecraft:raw_copper_block"),
                                               named("minecraft:tuff"),
                                               named("minecraft:deepslate_iron_ore"),
                                               named("minecraft:raw_iron_block")};
    const BlockState stone = named("minecraft:stone");
    const BlockState water = named("minecraft:water");
    const BlockState air = named("minecraft:air");
    const BlockState lava{.name = ResourceLocation::parse("minecraft:lava"),
                          .properties = {{"level", "0"}}};

    std::size_t veinPlaced = 0;
    std::size_t mixedRuns = 0;
    std::size_t lavaBlocks = 0;
    std::size_t waterBlocks = 0;
    for (const std::pair<std::int32_t, std::int32_t> chunk :
         {std::pair{0, 0}, std::pair{-2, 3}, std::pair{5, -7}}) {
        const std::int32_t chunkX = chunk.first;
        const std::int32_t chunkZ = chunk.second;
        CAPTURE(chunkX, chunkZ);
        ChunkBuffer buffer(geometry);
        filler.fill(chunkX, chunkZ, buffer);

        stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
        std::vector<BlockState> palette{air};
        std::size_t mismatched = 0;
        const std::int32_t topY = geometry.minY + geometry.height;
        // The fill's own order — cell by cell, then y, then z, then x — so
        // that the palette is rebuilt in the order blocks are first placed.
        for (std::int32_t cellZ = 0; cellZ < 16; cellZ += geometry.cellWidth()) {
            for (std::int32_t cellX = 0; cellX < 16; cellX += geometry.cellWidth()) {
                for (std::int32_t y = geometry.minY; y < topY; ++y) {
                    std::size_t solidInRun = 0;
                    for (std::int32_t z = cellZ; z < cellZ + geometry.cellWidth(); ++z) {
                        for (std::int32_t x = cellX; x < cellX + geometry.cellWidth(); ++x) {
                            const stratum::density::Point at{
                                .x = (chunkX * 16) + x, .y = y, .z = (chunkZ * 16) + z};
                            const double density =
                                interpreter.evaluate(entry(RouterEntry::FinalDensity), at, cache);
                            BlockState expected = air;
                            if (density > 0.0) {
                                ++solidInRun;
                                expected = stone;
                                if (stratum::ore::inAnyVeinRange(y)) {
                                    const double toggle = interpreter.evaluate(
                                        entry(RouterEntry::VeinToggle), at, cache);
                                    const stratum::ore::Vein vein =
                                        veins.at(at.x, y, at.z,
                                                 stratum::ore::VeinInputs{
                                                     .toggle = toggle,
                                                     .ridged = interpreter.evaluate(
                                                         entry(RouterEntry::VeinRidged), at, cache),
                                                     .gap = interpreter.evaluate(
                                                         entry(RouterEntry::VeinGap), at, cache)});
                                    if (vein.placed()) {
                                        ++veinPlaced;
                                        const std::size_t metal =
                                            vein.type == stratum::ore::VeinType::Copper ? 0U : 3U;
                                        std::size_t kind = 2U;
                                        if (vein.block == stratum::ore::VeinBlock::Filler) {
                                            kind = 0U;
                                        } else if (vein.block == stratum::ore::VeinBlock::Ore) {
                                            kind = 1U;
                                        }
                                        expected = veinBlocks[metal + kind];
                                    }
                                }
                            } else if (y < stratum::aquifer::lambdaLevel(settings.seaLevel)) {
                                expected = lava;
                                ++lavaBlocks;
                            } else if (y < settings.seaLevel) {
                                expected = water;
                                ++waterBlocks;
                            }
                            if (std::ranges::find(palette, expected) == palette.end()) {
                                palette.push_back(expected);
                            }
                            if (!(buffer.at(x, y, z) == expected)) {
                                ++mismatched;
                            }
                        }
                    }
                    if (solidInRun != 0U && solidInRun != 16U) {
                        ++mixedRuns;
                    }
                }
            }
        }
        CHECK(mismatched == 0U);
        CHECK(buffer.palette() == palette);
    }
    // The cases the runs must tell apart, each present.
    CHECK(veinPlaced > 0U);
    CHECK(mixedRuns > 100U);
    CHECK(lavaBlocks > 0U);
    CHECK(waterBlocks > 0U);
}
