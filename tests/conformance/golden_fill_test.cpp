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
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
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

    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), kSeed, stratum::density::RandomSource::Xoroshiro);
    const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, overworld);

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
