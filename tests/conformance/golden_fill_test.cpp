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
// TWO GRANULARITIES, because they measure different things.
//
//   * **Category** — solid, fluid or air — is what the FILLER decides, and it
//     is exact: 393216 of 393216 blocks. Nothing this build places is in the
//     wrong category anywhere in four chunks.
//   * **Exact block** is 82.013%, and every one of the remaining 17.987% is a
//     SURFACE RULE this build does not run yet: deepslate and bedrock (both
//     vertical gradients that reach the whole column, not a skin at the top),
//     and gravel, dirt and grass at the surface itself.
//
// Reporting only the second would read as terrain being one part in five
// wrong. Reporting only the first would hide that the world is bare stone.
//
// The off-by-one this comparison caught, which is why it is here: `sea_level`
// is EXCLUSIVE. With vanilla's 63 the water stops at 62. An inclusive
// comparison put one extra water block on top of every column — 256 a chunk,
// exactly the 1024 that showed up as the only category mismatch across four
// chunks — and nothing short of comparing against real blocks would have said
// so.
//
// SURFACE RULES ARE NOW WIRED IN (M4) — a resolved `surface::RuleGraph` and
// the overworld's own biome parameter table go into `ChunkFiller::compile`
// below — and the 82.013% above has not moved, because the overworld's own
// tree still names one thing this build cannot run it with: its single
// `temperature` condition, which needs a biome's own declared temperature
// and has nothing supplying one yet (SPEC §11). `bandlands`, the tree's
// other one-time blocker, closed — its colour table's construction is now
// derived clean-room and confirmed exactly against three world seeds and
// 532224 real blocks (spec/bandlands-spec.md) — so it no longer appears
// here at all. This is asserted directly (`runsSurfaceRules()` /
// `surfaceRulesBlockedBy()`) rather than left to be inferred from the count
// staying put, so the day `temperature` closes too, THIS assertion fails
// first and says why the numbers below moved rather than leaving that to be
// rediscovered.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/biome/parameter_list.hpp>
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

    // Not under `tree`: the pack ships only `{"preset": "minecraft:overworld"}`
    // for this — the real table is compiled into the jar, and
    // tools/fetch-vanilla asks the server's own data generator to dump it
    // instead (see biome/parameter_list.hpp).
    const std::filesystem::path parametersPath =
        fixtures() / "biome_parameters" / "minecraft" / "overworld.json";
    if (!std::filesystem::is_regular_file(parametersPath)) {
        SKIP("no biome parameter list at " << parametersPath);
    }
    std::ifstream parametersFile(parametersPath);
    std::stringstream parametersJson;
    parametersJson << parametersFile.rdbuf();
    const auto biomeParameters = stratum::biome::ParameterList::fromJson(
        nlohmann::json::parse(parametersJson.str()),
        stratum::data::ResourceLocation::parse("minecraft:overworld"));
    const auto surfaceRules = stratum::surface::RuleGraph::resolve(
        overworld.surfaceRule, stratum::data::ResourceLocation::parse("minecraft:overworld"));

    // referencedNoises() is the DENSITY graph's own contract; `bandlands`'
    // one registered noise is a SURFACE construct's need and has to be
    // asked for explicitly, same as ChunkFiller::compile's own doc already
    // says for minecraft:surface/minecraft:surface_secondary.
    auto wantedNoises = loaded.graph.referencedNoises();
    wantedNoises.push_back(stratum::data::ResourceLocation::parse("minecraft:clay_bands_offset"));
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, wantedNoises, kSeed, stratum::density::RandomSource::Xoroshiro);
    const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, overworld,
                                                               &surfaceRules, &biomeParameters);

    // See the file comment: wired in, still blocked, by name, on purpose —
    // now down to one reason: the overworld's single `temperature`
    // condition, which needs a biome's own declared temperature and has
    // nothing supplying one yet (ChunkFiller::compile's own doc).
    CHECK_FALSE(filler.runsSurfaceRules());
    REQUIRE(filler.surfaceRulesBlockedBy().size() == 1U);
    CHECK_THAT(filler.surfaceRulesBlockedBy().front(), ContainsSubstring("minecraft:temperature"));

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

    // Every block, in the right category. This is the filler's own claim and
    // it is exact — not a threshold, and not rounded.
    CHECK(sameCategory == blocks);

    // And the exact-block number, pinned. It moves when surface rules land,
    // and it should move upward; anything else means the filler changed.
    CHECK(exact == 322490U);
}
