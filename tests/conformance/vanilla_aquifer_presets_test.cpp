// Stratum — amplified and large_biomes against the server, where the
// overworld's goldens never reach.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The two presets are pure data to Stratum: the same ChunkFiller runs them,
// and vanilla_compiled_dimension_test.cpp shows the frozen blob fills what the
// pack fills. What that cannot show is whether the server agrees, and the
// presets reach aquifer regimes the eight golden overworld regions barely
// touch, beyond the preliminary surfaces (up to 141) the level rule was
// fitted on:
//
//   * AMPLIFIED, at seed 322: the local aquifer deciding blocks above sea
//     level from cells whose psl gate is above 141, and a y_skip cutoff
//     computed from a very high surface;
//   * AMPLIFIED, at seed 163: aquifer fluid and barrier above sea level;
//   * LARGE_BIOMES: Q5.9's deep-dark override fed by the preset's own erosion
//     and depth (overworld_large_biomes/*), read through the chunk's
//     flat_cache window.
//
// THE WORLDS are tools/analysis/aquifer-presets-probe.sh's: three dimensions
// in one frozen server start per seed, each naming vanilla's settings BY
// REFERENCE — aquifers, ore veins and every router entry as shipped — with the
// biome fixed to an empty one, so no carver or feature is ever what a mismatch
// is blamed on. The seeds and windows were chosen offline by
// tools/analysis/aquifer-presets-scout.cpp, from Stratum alone, because a
// blind window shows none of these regimes; each corpus's manifest records
// its windows and the case checks they are these. The overworld arm is the
// control, at amplified's window: the two share continents, erosion and
// ridges, so a disagreement amplified shows and the control does not belongs
// to the preset rather than to this harness.
//
// EVERY ARM is scored the same way, over all 64 chunks:
//
//   * the RAW first pass (density, aquifer, veins; no surface rule) against
//     the server's category, every disagreement required to take a shape
//     fluid flow leaves (support/fluid_flow.hpp) and the flow bounded;
//   * the SURFACED fill (the overworld's rule tree over the fixed biome)
//     against the server's block name wherever the category agrees — the
//     surface rules and the veins, including above_preliminary_surface's psl
//     lattice over amplified relief;
//   * floors on what the aquifer decides there at all, computed by Stratum
//     alone, so a window the aquifer barely touches cannot pass vacuously.
//
// Nothing this reads is committed: the fixtures are Mojang-derived (SPEC §12).
#include "support/aquifer_footprint.hpp"
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace {

using stratum::data::ResourceLocation;
using stratum::test::Category;

constexpr std::int32_t kChunks = 8;

/// One dimension of a probe corpus: its directory name, its settings, and its
/// window's first chunk.
struct Arm {
    std::int64_t seed;
    const char* name;
    const char* settings;
    std::int32_t originX;
    std::int32_t originZ;
    /// The flow-shaped disagreements two frozen server runs left, recorded
    /// rather than pinned: the bound is four times the larger, and never
    /// below 1 in 100 000 blocks.
    std::array<std::size_t, 2> flowRuns;
};

// The seeds and windows tools/analysis/aquifer-presets-scout.cpp chose, which
// tools/analysis/aquifer-probes.sh generates.
constexpr Arm kHighAmplified{.seed = 322,
                             .name = "amplified",
                             .settings = "minecraft:amplified",
                             .originX = 1,
                             .originZ = 7,
                             .flowRuns = {0, 0}};
constexpr Arm kHighLargeBiomes{.seed = 322,
                               .name = "large_biomes",
                               .settings = "minecraft:large_biomes",
                               .originX = 0,
                               .originZ = 19,
                               .flowRuns = {63, 63}};
constexpr Arm kHighControl{.seed = 322,
                           .name = "overworld",
                           .settings = "minecraft:overworld",
                           .originX = 1,
                           .originZ = 7,
                           .flowRuns = {0, 0}};
constexpr Arm kWetAmplified{.seed = 163,
                            .name = "amplified",
                            .settings = "minecraft:amplified",
                            .originX = 14,
                            .originZ = 23,
                            .flowRuns = {38, 38}};
constexpr Arm kWetLargeBiomes{.seed = 163,
                              .name = "large_biomes",
                              .settings = "minecraft:large_biomes",
                              .originX = 14,
                              .originZ = 21,
                              .flowRuns = {44, 44}};
constexpr Arm kWetControl{.seed = 163,
                          .name = "overworld",
                          .settings = "minecraft:overworld",
                          .originX = 14,
                          .originZ = 23,
                          .flowRuns = {86, 86}};

/// The highest psl the aquifer's level rule was fitted on (aquifer/lattice.hpp).
constexpr std::int32_t kFittedPslMax = 141;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

[[nodiscard]] std::filesystem::path corpus(const Arm& arm) {
    return fixtures() / "probes" / "aquifer-presets" / ("seed-" + std::to_string(arm.seed));
}

using Cell = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

/// What one arm's 64 chunks came to.
struct Score {
    std::size_t blocks = 0;
    std::size_t agree = 0;
    /// Raw-category disagreements in a shape fluid flow leaves.
    std::size_t flowShaped = 0;
    /// Those of them that are flowing fluid (`level` > 0).
    std::size_t flowing = 0;
    std::size_t unexplained = 0;
    /// The surfaced fill's block name against the server's, where the raw
    /// category agrees.
    std::size_t nameMismatch = 0;
    /// Aquifers on against off: where the aquifer decided what the global
    /// picker would not.
    stratum::test::AquiferFootprint footprint;
    /// Distinct nearest cells of the blocks the aquifer changed whose psl
    /// gate is above kFittedPslMax, and the blocks they changed.
    std::set<Cell> highGateCells;
    std::size_t highGateChanged = 0;
    /// Blocks at or above sea level the LOCAL aquifer decides — not solid by
    /// density, and at or below the chunk's y_skip — whatever it decides;
    /// those whose nearest cell's psl gate is above kFittedPslMax; and of
    /// those, the ones it changes. Under such a cell Stratum leaves almost
    /// every one of them air, the global picker's own answer, so a level rule
    /// that floods them is what the server would show.
    std::size_t aboveSeaDecided = 0;
    std::size_t aboveSeaDecidedHighGate = 0;
    std::size_t aboveSeaChangedHighGate = 0;
    /// Q5.9: blocks whose category the override decides (the shipped filler
    /// against one where it cannot hold), and which side the server takes.
    std::size_t deepDarkDecided = 0;
    std::size_t deepDarkShipped = 0;
    std::size_t deepDarkRival = 0;
    std::size_t deepDarkRivalNotFlow = 0;
};

[[nodiscard]] Category categoryOf(const stratum::settings::BlockState& block) {
    return stratum::test::categoryOf(block.name.toString());
}

/// Fails unless the corpus's manifest says @p arm was generated from the
/// settings and window this case scores.
void requireArm(const Arm& arm) {
    std::ifstream in(corpus(arm) / "manifest.json");
    REQUIRE(in.good());
    const nlohmann::json manifest = nlohmann::json::parse(in, nullptr, false);
    REQUIRE(!manifest.is_discarded());
    INFO(corpus(arm) << "'s manifest does not record " << arm.name << " as " << arm.settings
                     << " at chunk (" << arm.originX << ", " << arm.originZ
                     << "); regenerate it with tools/analysis/aquifer-probes.sh");
    REQUIRE(manifest.contains("dimensions"));
    REQUIRE(manifest.at("dimensions").contains(arm.name));
    const nlohmann::json& dimension = manifest.at("dimensions").at(arm.name);
    REQUIRE(dimension.at("settings") == arm.settings);
    REQUIRE(dimension.at("origin_chunk") == nlohmann::json::array({arm.originX, arm.originZ}));
    REQUIRE(manifest.at("chunks") == kChunks);
}

[[nodiscard]] Score score(const Arm& arm, const bool scoreDeepDark) {
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region = corpus(arm) / arm.name / "r.0.0.mca";
    stratum::test::requireFrozen(corpus(arm), "tools/analysis/aquifer-presets-probe.sh");
    stratum::test::requireSeed(corpus(arm), arm.seed);
    requireArm(arm);
    REQUIRE(std::filesystem::is_regular_file(region));

    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto id = ResourceLocation::parse(arm.settings);
    // Exactly as the probe world has them: by reference, ore veins included.
    const stratum::settings::NoiseSettings& settings = loaded.settings.at(id);
    const auto surfaceRules = stratum::surface::RuleGraph::resolve(settings.surfaceRule, id);

    // The probe's fixed biome, as golden_fill_aquifer_test.cpp builds it.
    const auto probeBiome = ResourceLocation::parse("stratum:probe");
    const nlohmann::json wholeClimate = nlohmann::json::array({-2.0, 2.0});
    const auto biomeParameters = stratum::biome::ParameterList::fromJson(
        nlohmann::json{{"biomes",
                        {{{"biome", probeBiome.toString()},
                          {"parameters",
                           {{"temperature", wholeClimate},
                            {"humidity", wholeClimate},
                            {"continentalness", wholeClimate},
                            {"erosion", wholeClimate},
                            {"depth", wholeClimate},
                            {"weirdness", wholeClimate},
                            {"offset", 0.0}}}}}}},
        probeBiome);
    const std::filesystem::path probeBiomeTree = stratum::test::tempPath(
        "stratum-aquifer-presets-biome-" + std::to_string(arm.seed) + "-" + arm.name);
    std::filesystem::remove_all(probeBiomeTree);
    std::filesystem::create_directories(probeBiomeTree / "biome");
    {
        std::ofstream probeBiomeFile(probeBiomeTree / "biome" / "probe.json");
        probeBiomeFile << nlohmann::json{{"temperature", 0.8}}.dump();
    }
    const auto biomeTemperatures = stratum::biome::TemperatureTable::fromPack(
        stratum::data::Pack::openWorldgenTree(probeBiomeTree, "stratum"));
    std::filesystem::remove_all(probeBiomeTree);

    auto wantedNoises = loaded.graph.referencedNoises();
    const auto surfaceNoises = surfaceRules.referencedNoises();
    wantedNoises.insert(wantedNoises.end(), surfaceNoises.begin(), surfaceNoises.end());
    for (const char* noise :
         {"minecraft:surface", "minecraft:surface_secondary", "minecraft:clay_bands_offset"}) {
        wantedNoises.push_back(ResourceLocation::parse(noise));
    }
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, wantedNoises, arm.seed, stratum::density::RandomSource::Xoroshiro);

    const auto raw = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings);
    const auto surfaced = stratum::terrain::ChunkFiller::compile(
        loaded.graph, noises, settings, &surfaceRules, &biomeParameters, &biomeTemperatures);
    REQUIRE(surfaced.runsSurfaceRules());
    stratum::settings::NoiseSettings pickerSettings = settings;
    pickerSettings.aquifersEnabled = false;
    const auto picker =
        stratum::terrain::ChunkFiller::compile(loaded.graph, noises, pickerSettings);
    // Q5.9's rival: `depth` read as `erosion` can never be both below -0.225
    // and above 0.9, so the override never holds; every other read, y_skip and
    // the veins included, is the shipped one.
    stratum::settings::NoiseSettings rivalSettings = settings;
    rivalSettings.router.entries[static_cast<std::size_t>(stratum::settings::RouterEntry::Depth)] =
        settings.router.at(stratum::settings::RouterEntry::Erosion);
    const auto rival = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, rivalSettings);

    // The psl reads the power figures need, through each chunk's window as
    // the filler reads them.
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                       .height = settings.geometry.cellHeight()});
    const auto pslNode =
        settings.router.at(stratum::settings::RouterEntry::PreliminarySurfaceLevel);
    const stratum::aquifer::CentreSource centres(arm.seed);
    const std::int32_t sea = settings.seaLevel;

    stratum::test::GoldenRegion golden(region);
    Score result;
    std::size_t reported = 0;
    for (std::int32_t chunkZ = arm.originZ; chunkZ < arm.originZ + kChunks; ++chunkZ) {
        for (std::int32_t chunkX = arm.originX; chunkX < arm.originX + kChunks; ++chunkX) {
            REQUIRE(golden.hasChunk(chunkX, chunkZ));
            stratum::terrain::ChunkBuffer first(settings.geometry);
            raw.fill(chunkX, chunkZ, first);
            stratum::terrain::ChunkBuffer withSurface(settings.geometry);
            surfaced.fill(chunkX, chunkZ, withSurface);
            stratum::terrain::ChunkBuffer withoutAquifer(settings.geometry);
            picker.fill(chunkX, chunkZ, withoutAquifer);
            std::optional<stratum::terrain::ChunkBuffer> rivalFirst;
            if (scoreDeepDark) {
                rivalFirst.emplace(settings.geometry);
                rival.fill(chunkX, chunkZ, *rivalFirst);
            }
            stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
            const stratum::density::FlatCacheWindow window =
                stratum::terrain::ChunkFiller::flatCacheWindow(chunkX, chunkZ);
            const auto psl = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
                return interpreter.evaluate(
                    pslNode, stratum::density::Point{.x = x, .y = y, .z = z}, cache, window);
            };
            // The chunk's y_skip, as ChunkFiller::fill computes it: above it
            // the global picker decides and the local aquifer is not asked.
            const stratum::aquifer::YSkipRectangle rectangle =
                stratum::aquifer::ySkipRectangle(chunkX * 16, chunkZ * 16);
            std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
            for (std::int32_t z = rectangle.minZ; z <= rectangle.maxZ;
                 z += stratum::aquifer::kYSkipSampleStride) {
                for (std::int32_t x = rectangle.minX; x <= rectangle.maxX;
                     x += stratum::aquifer::kYSkipSampleStride) {
                    maxSurface = std::max(
                        maxSurface, stratum::javamath::floorToInt(
                                        psl(x, stratum::aquifer::kPreliminarySurfaceSampleY, z)));
                }
            }
            const std::int32_t ySkip = stratum::aquifer::ySkip(maxSurface);
            // One scan per centre per chunk, the filler's StatusCache scope.
            std::map<Cell, std::int32_t> gates;

            for (std::int32_t y = settings.geometry.minY; y < settings.geometry.maxY(); ++y) {
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        const auto* theirs = golden.blockAt(x, y, z);
                        const std::string theirName =
                            theirs != nullptr ? theirs->name : std::string("minecraft:air");
                        const Category g = stratum::test::categoryOf(theirName);
                        const Category r = categoryOf(first.at(localX, y, localZ));
                        const Category off = categoryOf(withoutAquifer.at(localX, y, localZ));
                        ++result.blocks;
                        result.footprint.add(off, r, y, sea);

                        const bool explained =
                            g != r && stratum::test::explainedByFlow(golden, x, y, z, g, r);
                        if (g == r) {
                            ++result.agree;
                            const std::string ours =
                                withSurface.at(localX, y, localZ).name.toString();
                            if (ours != theirName) {
                                ++result.nameMismatch;
                                if (reported++ < 10) {
                                    UNSCOPED_INFO("surfaced " << ours << ", server " << theirName
                                                              << " at " << x << " " << y << " "
                                                              << z);
                                }
                            }
                        } else if (explained) {
                            ++result.flowShaped;
                            result.flowing += stratum::test::fluidLevel(theirs) > 0 ? 1U : 0U;
                        } else {
                            ++result.unexplained;
                            if (reported++ < 10) {
                                UNSCOPED_INFO("unexplained at " << x << " " << y << " " << z
                                                                << ": server " << theirName);
                            }
                        }

                        if (off != Category::Solid && y <= ySkip) {
                            const stratum::aquifer::Source nearest =
                                stratum::aquifer::selectSources(centres, x, y, z).nearest();
                            const Cell key{nearest.centre.x, nearest.centre.y, nearest.centre.z};
                            auto gate = gates.find(key);
                            if (gate == gates.end()) {
                                gate = gates
                                           .emplace(key, stratum::aquifer::readPreliminarySurface(
                                                             psl, nearest.centre, sea)
                                                             .gate)
                                           .first;
                            }
                            const bool highGate = gate->second > kFittedPslMax;
                            if (y >= sea) {
                                ++result.aboveSeaDecided;
                                result.aboveSeaDecidedHighGate += highGate ? 1U : 0U;
                                result.aboveSeaChangedHighGate += highGate && off != r ? 1U : 0U;
                            }
                            if (highGate && off != r) {
                                result.highGateCells.insert(key);
                                ++result.highGateChanged;
                            }
                        }

                        if (rivalFirst.has_value()) {
                            const Category rivalCategory =
                                categoryOf(rivalFirst->at(localX, y, localZ));
                            if (rivalCategory != r) {
                                ++result.deepDarkDecided;
                                if (g == r) {
                                    ++result.deepDarkShipped;
                                } else if (g == rivalCategory) {
                                    ++result.deepDarkRival;
                                    result.deepDarkRivalNotFlow += explained ? 0U : 1U;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

/// What every arm is held to.
void checkArm(const Arm& arm, const Score& result) {
    WARN(arm.name << " at seed " << arm.seed << ": " << result.agree << " of " << result.blocks
                  << " raw categories agree; flow-shaped " << result.flowShaped << " ("
                  << result.flowing << " flowing), unexplained " << result.unexplained
                  << "; surfaced names wrong " << result.nameMismatch << "; aquifer footprint "
                  << result.footprint.total << " (barrier " << result.footprint.barrier << ", dry "
                  << result.footprint.dry << ", local lava " << result.footprint.localLava
                  << ", above sea " << result.footprint.aboveSea << "); above sea the aquifer "
                  << "decides " << result.aboveSeaDecided << " blocks, "
                  << result.aboveSeaDecidedHighGate << " under a cell gated above " << kFittedPslMax
                  << " (" << result.aboveSeaChangedHighGate << " changed); "
                  << result.highGateCells.size() << " such cells change " << result.highGateChanged
                  << " blocks");
    REQUIRE(result.blocks == 6291456U);
    // Not vacuous: the aquifer decides a real share of this window.
    REQUIRE(result.footprint.total >= 10000U);
    CHECK(result.footprint.other == 0U);
    // Every raw disagreement is fluid that moved after generating, and the
    // flow is bounded rather than pinned: it differs between frozen runs.
    CHECK(result.unexplained == 0U);
    CHECK(result.flowShaped <=
          std::max(result.blocks / 100000, 4 * std::max(arm.flowRuns[0], arm.flowRuns[1])));
    // Surface rules and veins, wherever the first pass agrees.
    CHECK(result.nameMismatch == 0U);
}

/// SKIPs the running case unless @p arm's corpus is on disk.
void requireCorpus(const Arm& arm) {
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_regular_file(corpus(arm) / "manifest.json")) {
        SKIP("no aquifer-presets probe at " << corpus(arm) << "; generate it with "
                                            << "tools/analysis/aquifer-probes.sh --accept-eula");
    }
}

/// Q5.9 on a large_biomes arm: enough of the override in the window to be
/// seen, and the server never siding with its absence except where fluid
/// moved.
void checkDeepDark(const Arm& arm, const Score& result) {
    WARN("large_biomes at seed " << arm.seed << ": Q5.9 decides " << result.deepDarkDecided
                                 << " blocks; the server sides with the override on "
                                 << result.deepDarkShipped << ", with its absence on "
                                 << result.deepDarkRival << " (" << result.deepDarkRivalNotFlow
                                 << " not flow)");
    REQUIRE(result.deepDarkDecided >= 1000U);
    CHECK(result.deepDarkRivalNotFlow == 0U);
}

} // namespace

TEST_CASE("amplified against the server: the aquifer above the sea under a psl above 141",
          "[conformance][terrain][aquifer]") {
    requireCorpus(kHighAmplified);
    const Score result = score(kHighAmplified, false);
    checkArm(kHighAmplified, result);
    // The regime this window was chosen for, by Stratum's own count: cells
    // gated above anything the level rule was fitted on, deciding blocks
    // above the sea and changing blocks below it.
    REQUIRE(result.aboveSeaDecidedHighGate >= 10000U);
    REQUIRE(result.highGateCells.size() >= 50U);
}

TEST_CASE("amplified against the server: aquifer fluid and barrier above the sea",
          "[conformance][terrain][aquifer]") {
    requireCorpus(kWetAmplified);
    const Score result = score(kWetAmplified, false);
    checkArm(kWetAmplified, result);
    REQUIRE(result.footprint.aboveSea >= 1000U);
}

TEST_CASE("large_biomes against the server: Q5.9 fed by the preset's own erosion and depth",
          "[conformance][terrain][aquifer]") {
    requireCorpus(kHighLargeBiomes);
    const Score result = score(kHighLargeBiomes, true);
    checkArm(kHighLargeBiomes, result);
    checkDeepDark(kHighLargeBiomes, result);
}

TEST_CASE("large_biomes against the server: Q5.9 and aquifer fluid above the sea",
          "[conformance][terrain][aquifer]") {
    requireCorpus(kWetLargeBiomes);
    const Score result = score(kWetLargeBiomes, true);
    checkArm(kWetLargeBiomes, result);
    checkDeepDark(kWetLargeBiomes, result);
    REQUIRE(result.footprint.aboveSea >= 1000U);
}

TEST_CASE("the overworld control at amplified's window at seed 322",
          "[conformance][terrain][aquifer]") {
    requireCorpus(kHighControl);
    checkArm(kHighControl, score(kHighControl, false));
}

TEST_CASE("the overworld control at amplified's window at seed 163",
          "[conformance][terrain][aquifer]") {
    requireCorpus(kWetControl);
    const Score result = score(kWetControl, false);
    checkArm(kWetControl, result);
    REQUIRE(result.footprint.aboveSea >= 1000U);
}
