// Stratum — the chunk filler's aquifer wiring against real, aquifer-ON blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `golden_fill_test.cpp` validates the filler with aquifers held OFF. This
// validates the wiring SPEC §10's MA milestone landed (`ChunkFiller` calling
// `aquifer::computeSubstance`): the reference is
// `tools/analysis/aquifer-on-probe.sh`'s world — vanilla's real overworld
// settings, aquifers and all, with only `ore_veins_enabled` forced off (not
// implemented) and the same empty-biome swap the aquifer-free sibling uses,
// so a carver or a feature is never what a mismatch is blamed on.
//
// TWO FILLERS, on the SAME region, because they isolate different layers.
//
//   * **RAW** — `ChunkFiller::compile` given no `surface::RuleGraph` at all,
//     so `fill()` never runs past the first pass: density, then the aquifer,
//     then air. Category (solid/fluid/air) against this is EXACT on the same
//     four chunks `golden_fill_test.cpp` uses: 393216 of 393216. This is the
//     number that actually says whether the wiring is right — the cell
//     lattice, the centre jitter, `cellFluidLevel`, `selectSources` and
//     `placesBarrier`, all running against real generation for the first
//     time, not a purpose-built void-column probe.
//
//   * **WITH SURFACE RULES** — the overworld's real 287-rule tree, same as
//     `golden_fill_test.cpp`. Category is 393216 of 393216 — exact, both
//     passes. It USED to stop at 392755 (99.883%, 461 short), and the
//     shortfall was never a new gap: it was `golden_fill_test.cpp`'s own
//     already-documented `deepslate` residual, reached far more often
//     because a real aquifer carves genuinely different terrain — air
//     pockets and drained cells a flat sea level never produces — for that
//     same unconditioned rule to misfire on. 451 of the 461 mismatches were
//     air the unconditioned rule painted solid; only 10 were fluid. That
//     told against the first fix `golden_fill_test.cpp`'s own residual
//     took: a FLUID-only guard (`Context::stoneDepthAbove == 0`) closed the
//     475-block aquifer-free gap but left every one of these 451 air
//     mismatches standing, since air resets `stoneDepthAbove` on purpose
//     (Context's own doc) — indistinguishable, by that field alone, from
//     the column's own open sky. The general fix in
//     `ChunkFiller::applySurfaceRules` tracks a monotonic "solid crossed
//     yet" flag instead, covering fluid and air alike, and closes this to
//     393216 exact together with `golden_fill_test.cpp`'s own 393216.
//     Confirmed by construction: every one of the RAW pass's 393216
//     categories was already exact, so nothing past the first pass was
//     ever the aquifer's to answer for.
//
// Nothing this reads is committed: the fixture is Mojang-derived (SPEC §12).
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr std::int64_t kSeed = -1;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// Solid, fluid or air. The three the filler chooses between.
[[nodiscard]] std::string categoryOf(const std::string& name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air") {
        return "air";
    }
    if (name == "minecraft:water" || name == "minecraft:lava") {
        return "fluid";
    }
    return "solid";
}

} // namespace

TEST_CASE("the aquifer wiring places the blocks the server placed, before any surface rule",
          "[conformance][terrain][aquifer]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region =
        fixtures() / "probes" / "aquifer-on" / ("seed-" + std::to_string(kSeed)) / "r.0.0.mca";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_regular_file(region)) {
        SKIP("no aquifer-on probe at " << region << "; generate it with "
                                       << "tools/analysis/aquifer-on-probe.sh --accept-eula");
    }

    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    auto overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    // The probe world is vanilla's overworld with aquifers left ON — the
    // one field the aquifer-free sibling flips is exactly the one this test
    // exists to NOT flip. Ore veins stay off; they are not implemented.
    overworld.oreVeinsEnabled = false;

    const auto surfaceRules = stratum::surface::RuleGraph::resolve(
        overworld.surfaceRule, stratum::data::ResourceLocation::parse("minecraft:overworld"));

    const auto probeBiome = stratum::data::ResourceLocation::parse("stratum:probe");
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

    const std::filesystem::path probeBiomeTree =
        std::filesystem::temp_directory_path() / "stratum-golden-fill-aquifer-probe-biome";
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
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:surface"));
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:surface_secondary"));
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:clay_bands_offset"));
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, wantedNoises, kSeed, stratum::density::RandomSource::Xoroshiro);

    // No surface::RuleGraph passed: the RAW first pass only, isolating the
    // aquifer's own decision from the deepslate gap below.
    const auto rawFiller = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, overworld);
    const auto filler = stratum::terrain::ChunkFiller::compile(
        loaded.graph, noises, overworld, &surfaceRules, &biomeParameters, &biomeTemperatures);
    CHECK(filler.runsSurfaceRules());
    CHECK(filler.surfaceRulesBlockedBy().empty());

    const auto file = stratum::region::RegionFile::open(region);

    std::size_t blocks = 0;
    std::size_t exact = 0;
    std::size_t sameCategory = 0;
    std::size_t rawSameCategory = 0;
    std::size_t chunks = 0;

    // Chunks 0..1 in both axes — the same four `golden_fill_test.cpp` reads,
    // inside every probe's own forceloaded window.
    for (std::int32_t chunkZ = 0; chunkZ < 2; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 2; ++chunkX) {
            REQUIRE(file.hasChunk(chunkX, chunkZ));
            const auto golden = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);

            stratum::terrain::ChunkBuffer buffer(overworld.geometry);
            filler.fill(chunkX, chunkZ, buffer);
            stratum::terrain::ChunkBuffer rawBuffer(overworld.geometry);
            rawFiller.fill(chunkX, chunkZ, rawBuffer);
            ++chunks;

            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (std::int32_t y = overworld.geometry.minY;
                         y < overworld.geometry.minY + overworld.geometry.height; ++y) {
                        const std::string ours = buffer.at(localX, y, localZ).name.toString();
                        const std::string raw = rawBuffer.at(localX, y, localZ).name.toString();
                        // A section the golden dropped for being all air is
                        // air, not absent.
                        const auto* theirBlock = golden.blockAt(localX, y, localZ);
                        const std::string theirs =
                            theirBlock != nullptr ? theirBlock->name : std::string("minecraft:air");

                        ++blocks;
                        if (ours == theirs) {
                            ++exact;
                            ++sameCategory;
                        } else if (categoryOf(ours) == categoryOf(theirs)) {
                            ++sameCategory;
                        }
                        if (categoryOf(raw) == categoryOf(theirs)) {
                            ++rawSameCategory;
                        }
                    }
                }
            }
        }
    }

    REQUIRE(chunks == 4U);
    REQUIRE(blocks == 393216U);

    // The wiring itself, isolated from every surface-rule complication:
    // every one of 393216 blocks' category is exactly what the real server
    // placed. This is the number that regresses if the aquifer wiring
    // breaks; the two below can move for reasons that have nothing to do
    // with it.
    CHECK(rawSameCategory == 393216U);

    // With the real 287-rule surface tree running: pinned, not bounded,
    // same reasoning as golden_fill_test.cpp — and now equal to `blocks`
    // itself. Anything short of 393216 here means either the aquifer wiring
    // regressed or the buried-fluid/buried-air gap the file comment
    // describes reopened.
    CHECK(sameCategory == 393216U);
    CHECK(exact == 393216U);
}
