// Stratum — the chunk filler against blocks the vanilla server wrote.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The reference is the aquifer-free world from
// tools/analysis/aquifer-free-probe.sh: vanilla's own overworld noise settings
// with `aquifers_enabled: false` and a biome carrying no carvers and no
// features. Both matter. Without the flag the blocks are not a function of
// `final_density` and the filler refuses the dimension outright; without the
// empty biome, carvers cut caves and features drop lakes on top, and neither
// is terrain.
//
// TWO GRANULARITIES, because they measure different things — and both are
// exact: 393216 of 393216 blocks, category AND exact block alike.
//
// The exact-block number USED to stop at 392741 (99.879%), 475 short. Every
// straggler was `deepslate`'s own rule (the overworld tree's third top-level
// sequence entry) — a bare `vertical_gradient` with nothing else gating it —
// firing by Y alone over FLUID it should have left alone, all between y -40
// and y 0, all inside this aquifer-free probe's deep ocean trenches. The
// natural read was that `above_preliminary_surface` (the entry above it) was
// the missing gate; it is not. Confirmed against the real server (SPEC §11):
// a datapack whose ENTIRE surface_rule is that one bare rule — no
// above_preliminary_surface, no bedrock floor, nothing else in the tree —
// still leaves exactly the same 475 fluid blocks untouched, and every one of
// them sits inside a fluid-filled cave-void with solid rock already crossed
// above it in the same column. The real gate is `ChunkFiller`'s own second
// pass, not anything the rule tree can express: a NON-SOLID position (fluid
// or air alike) stays eligible for surface rules only while it is part of
// the column's FIRST (topmost, reached straight from the sky) non-solid
// stretch — closed in `applySurfaceRules` (terrain_filler.cpp) with a plain
// monotonic "has solid been crossed yet" flag, deliberately not
// `stoneDepthAbove`, which resets on air on purpose and so cannot tell a
// deep cave's own air from the open sky. That distinction is invisible here
// — this probe runs `aquifers_enabled: false`, so every non-solid position
// below sea level is fluid, never air — and only showed up once the same
// fix ran against a real aquifer's own air pockets
// (golden_fill_aquifer_test.cpp, SPEC §11: 451 of that file's 461 mismatches
// were air, not fluid, under an earlier fluid-only version of this guard).
// The topmost/open-water case a rule keyed on `water` needs — freezing ice
// onto a lake's own surface — stays reachable; see the pre-existing "water
// reads the filler's own latched height" unit test, and
// `terrain_filler_test.cpp`'s "an unconditioned rule never rewrites a buried
// fluid pocket's own fluid" for the isolated buried-fluid regression.
//
// The off-by-one this comparison caught, which is why it is here: `sea_level`
// is EXCLUSIVE. With vanilla's 63 the water stops at 62. An inclusive
// comparison put one extra water block on top of every column — 256 a chunk,
// exactly the 1024 that showed up as the only category mismatch across four
// chunks — and nothing short of comparing against real blocks would have said
// so.
//
// SURFACE RULES NOW RUN (M4). The overworld's own tree named one last thing
// `ChunkFiller` could not supply — its single `temperature` condition needs
// a biome's own DECLARED value, and `biome::TemperatureTable` now resolves
// one (from the same `worldgen/biome` entries `pack` already parsed). This
// is the first time the real 287-rule, 141-condition tree has run end to end
// against real blocks, and it caught a real bug immediately: `stone_depth`
// compared a stored run counter that is legitimately 0 off the top of any
// solid run (Context's own doc — reset by air) as `0 - 1 = -1`, which
// trivially satisfies almost any non-negative threshold. Before the fix,
// every grass/dirt/sand composition rule this tree contains fired at EVERY
// position above the terrain, not just the top of it — grass painted the
// entire sky, and deepslate painted over open water — 292005 of 393216
// blocks in four chunks, caught by this very assertion. `Executor::test`'s
// `ConditionType::StoneDepth` case now refuses outright when the run counter
// is 0, closing that. The 475-block residual the file comment above
// describes was a second, much narrower gap the same run surfaced — closed
// too, not in the rule tree but in the pass around it.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
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
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;

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

TEST_CASE("the filler places the blocks the server placed, up to surface rules",
          "[conformance][terrain]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region =
        fixtures() / "probes" / "no-aquifer" / ("seed-" + std::to_string(kSeed)) / "r.0.0.mca";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_regular_file(region)) {
        SKIP("no aquifer-free probe at " << region << "; generate it with "
                                         << "tools/analysis/aquifer-free-probe.sh --accept-eula");
    }

    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    auto overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    // The probe world is vanilla's overworld with exactly these two off, so
    // the settings compared against are vanilla's in every other respect.
    overworld.aquifersEnabled = false;
    overworld.oreVeinsEnabled = false;

    const auto surfaceRules = stratum::surface::RuleGraph::resolve(
        overworld.surfaceRule, stratum::data::ResourceLocation::parse("minecraft:overworld"));

    // The probe world's own generator sets `biome_source` to `minecraft:fixed`,
    // pinned to one synthetic biome everywhere — `stratum:probe`, declaring
    // `temperature: 0.8` (tools/analysis/aquifer-free-probe.sh) — rather than
    // running the overworld's real multi-noise search. Resolving through the
    // overworld's own 7593-entry table here would answer a question vanilla
    // never asked when this region was generated, so this builds a
    // ParameterList with one entry spanning the whole climate space instead —
    // the same answer a `minecraft:fixed` source gives without searching at
    // all. [-2, 2] on every axis is wider than any real sample reaches
    // (biome::QuantizedPoint::fitness's own doc), so nothing here can fall
    // outside it.
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

    // Its own declared temperature, read the same way a real `worldgen/biome`
    // entry would be — just from a scratch tree rather than the fixture
    // pack, since the synthetic `stratum:probe` biome is never in it.
    const std::filesystem::path probeBiomeTree =
        std::filesystem::temp_directory_path() / "stratum-golden-fill-probe-biome";
    std::filesystem::remove_all(probeBiomeTree);
    std::filesystem::create_directories(probeBiomeTree / "biome");
    {
        std::ofstream probeBiomeFile(probeBiomeTree / "biome" / "probe.json");
        probeBiomeFile << nlohmann::json{{"temperature", 0.8}}.dump();
    }
    const auto biomeTemperatures = stratum::biome::TemperatureTable::fromPack(
        stratum::data::Pack::openWorldgenTree(probeBiomeTree, "stratum"));
    std::filesystem::remove_all(probeBiomeTree);

    // referencedNoises() is the DENSITY graph's own contract and knows
    // nothing about what the SURFACE graph needs — its own
    // referencedNoises() covers the noises its `noise_threshold` conditions
    // name, but `minecraft:surface`/`minecraft:surface_secondary`
    // (read by every `stone_depth` condition) and
    // `minecraft:clay_bands_offset` (`bandlands`) are needs of a RULE or
    // CONDITION type itself rather than a name any condition carries, so
    // Executor::compile's own doc asks for them explicitly.
    auto wantedNoises = loaded.graph.referencedNoises();
    const auto surfaceNoises = surfaceRules.referencedNoises();
    wantedNoises.insert(wantedNoises.end(), surfaceNoises.begin(), surfaceNoises.end());
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:surface"));
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:surface_secondary"));
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:clay_bands_offset"));
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, wantedNoises, kSeed, stratum::density::RandomSource::Xoroshiro);
    const auto filler = stratum::terrain::ChunkFiller::compile(
        loaded.graph, noises, overworld, &surfaceRules, &biomeParameters, &biomeTemperatures);

    // See the file comment: wired in, and now RUNNING — the overworld's
    // tree named one last thing ChunkFiller could not supply, and the
    // biome-temperature loader above closes it.
    CHECK(filler.runsSurfaceRules());
    CHECK(filler.surfaceRulesBlockedBy().empty());

    const auto file = stratum::region::RegionFile::open(region);

    std::size_t blocks = 0;
    std::size_t exact = 0;
    std::size_t sameCategory = 0;
    std::size_t chunks = 0;

    // Chunks 0..1 in both axes, which are inside the probe's forceloaded
    // window and so are fully generated. The region holds far more chunks than
    // that, most of them in an early status with no block data at all, and
    // iterating the sector table would diff terrain against empty.
    for (std::int32_t chunkZ = 0; chunkZ < 2; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 2; ++chunkX) {
            REQUIRE(file.hasChunk(chunkX, chunkZ));
            const auto golden = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);

            stratum::terrain::ChunkBuffer buffer(overworld.geometry);
            filler.fill(chunkX, chunkZ, buffer);
            ++chunks;

            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (std::int32_t y = overworld.geometry.minY;
                         y < overworld.geometry.minY + overworld.geometry.height; ++y) {
                        const std::string ours = buffer.at(localX, y, localZ).name.toString();
                        // A section the golden dropped for being all air is
                        // air, not absent: the y range comes from the
                        // dimension's geometry, never from what survived.
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
                    }
                }
            }
        }
    }

    REQUIRE(chunks == 4U);
    REQUIRE(blocks == 393216U);

    // Category, pinned to `blocks` itself — exact, not thresholded, so a
    // regression here shows up immediately rather than hiding under a
    // percentage.
    CHECK(sameCategory == 393216U);

    // The exact-block number, pinned to `blocks` too. This is the first run
    // of the whole 287-rule, 141-condition tree end to end, and every one of
    // the 393216 category matches is ALSO an exact match — the 475-block
    // buried-fluid gap the file comment describes accounted for the entire
    // shortfall until `applySurfaceRules` stopped reaching a fluid position
    // once solid had already been crossed above it. Anything short of
    // 393216 here means the filler changed.
    CHECK(exact == 393216U);
}
