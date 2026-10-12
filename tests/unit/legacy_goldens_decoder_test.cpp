// Stratum — the golden decoder's column reconstruction, without fixtures.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/analysis/legacy-goldens-surface-decoder.hpp reads a surface noise's
// sign out of a golden region by rebuilding, from the region alone, what the
// chunk filler's surface pass read: each block's category, the two
// stone-depth runs, the water height, and which positions the pass visits.
// Its only other checks need the golden regions (vanilla_legacy_goldens_
// surface_test.cpp, which skips in CI). These hold the reconstruction on
// synthetic chunks, and every one of them fails on the decoder before it
// followed pipeline engine v7 (SPEC §11, "Lava in the surface pass's runs"):
// lava Solid in a water dimension, fluid holding the bottom-up run, and every
// position of a column's first non-solid stretch visited.

#include "legacy-goldens-surface-decoder.hpp"
#include "support/chunk_builder.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using legacy_goldens::Category;
using legacy_goldens::GoldenChunk;
using stratum::settings::BlockState;
using stratum::settings::NoiseSettings;
using stratum::test::SectionSpec;

[[nodiscard]] BlockState state(const std::string& name, const std::string& level = "") {
    BlockState block{.name = stratum::data::ResourceLocation::parse(name), .properties = {}};
    if (!level.empty()) {
        block.properties.emplace("level", level);
    }
    return block;
}

/// A 32-block dimension at y 0..31, so a chunk is two sections.
[[nodiscard]] NoiseSettings dimension(const std::string& defaultBlock,
                                      const std::string& defaultFluid, bool oreVeins) {
    NoiseSettings settings;
    settings.defaultBlock = state(defaultBlock);
    settings.defaultFluid = state(defaultFluid, "0");
    settings.oreVeinsEnabled = oreVeins;
    settings.geometry.minY = 0;
    settings.geometry.height = 32;
    return settings;
}

/// Section 0 of a chunk whose columns (x, 0) are given bottom up, one
/// palette entry per block; everything else, and all of section 1, is air.
[[nodiscard]] GoldenChunk chunkOf(const std::vector<std::vector<std::string>>& columns,
                                  const NoiseSettings& settings) {
    SectionSpec lower;
    lower.y = 0;
    lower.blocks.assign(4096, 0);
    for (std::size_t x = 0; x < columns.size(); ++x) {
        for (std::size_t y = 0; y < columns[x].size(); ++y) {
            const std::string& block = columns[x][y];
            std::size_t entry = 0;
            while (entry < lower.palette.size() && lower.palette[entry] != block) {
                ++entry;
            }
            if (entry == lower.palette.size()) {
                lower.palette.push_back(block);
            }
            lower.blocks[(y * 256U) + x] = static_cast<std::uint16_t>(entry);
        }
    }
    const stratum::test::NbtWriter nbt = stratum::test::buildChunkNbt(
        0, 0, {lower, stratum::test::uniformSection(1, "minecraft:air")});
    return GoldenChunk{stratum::chunk::Chunk::decode(stratum::nbt::read(nbt.span()).root),
                       settings};
}

/// The y the decoder visits in column (x, 0), top down.
[[nodiscard]] std::vector<std::int32_t> visited(const GoldenChunk& chunk, int x, bool oreVeins) {
    std::vector<std::int32_t> ys;
    legacy_goldens::forEachSurfacePosition(chunk, x, 0, oreVeins,
                                           [&](std::int32_t y, Category) { ys.push_back(y); });
    return ys;
}

const std::string kStone = "minecraft:stone";
const std::string kLava = "minecraft:lava[level=0]";
const std::string kWater = "minecraft:water[level=0]";

} // namespace

TEST_CASE("the golden decoder counts a column's runs with the filler's own rules",
          "[legacy][goldens][decoder]") {
    // Bottom up: stone 0..3, aquifer lava 4..5, stone 6..9, water 10..11,
    // stone 12..15, air above. In a WATER dimension, where the old decoder
    // called that lava Solid.
    const NoiseSettings overworld = dimension(kStone, "minecraft:water", true);
    const std::vector<std::string> column{kStone, kStone, kStone, kStone, kLava,  kLava,
                                          kStone, kStone, kStone, kStone, kWater, kWater,
                                          kStone, kStone, kStone, kStone};
    const GoldenChunk chunk = chunkOf({column}, overworld);

    CHECK(chunk.categoryAt(0, 4, 0) == Category::Fluid);
    CHECK(chunk.categoryAt(0, 10, 0) == Category::Fluid);
    CHECK(chunk.categoryAt(0, 20, 0) == Category::Air);
    // Bottom up, both pools reset the run: 1 on each roof (the old decoder
    // read 7 over the lava, counting it, and 11 over the water, holding).
    CHECK(chunk.depthBelow(0, 6, 0) == 1);
    CHECK(chunk.depthBelow(0, 9, 0) == 4);
    CHECK(chunk.depthBelow(0, 12, 0) == 1);
    // Top down, both pools hold it: 9 under the lava (the old decoder: 11).
    CHECK(chunk.depthAbove(0, 12, 0) == 4);
    CHECK(chunk.depthAbove(0, 6, 0) == 8);
    CHECK(chunk.depthAbove(0, 3, 0) == 9);
    // The water height latches at the water, the topmost fluid.
    CHECK(chunk.waterHeightLow(0, 0) == std::optional<std::int32_t>{12});
    CHECK(chunk.scanFrom(0, 0) == 15);
    // Only what the first pass left as default_block is visited: never the
    // lava, which the old decoder visited as solid.
    CHECK(visited(chunk, 0, true) ==
          std::vector<std::int32_t>{15, 14, 13, 12, 9, 8, 7, 6, 3, 2, 1, 0});
}

TEST_CASE("the golden decoder visits no fluid, and no vein block, even at a column's top",
          "[legacy][goldens][decoder]") {
    const NoiseSettings overworld = dimension(kStone, "minecraft:water", true);
    // x 0: open water over a sand floor with an iron vein block in it. The
    // old decoder visited the water too — the first non-solid stretch — where
    // the leaf placing nothing always matched and decoded every placing
    // condition as false.
    const GoldenChunk chunk = chunkOf(
        {{kStone, "minecraft:deepslate_iron_ore", "minecraft:sand", kWater, kWater}}, overworld);
    CHECK(chunk.scanFrom(0, 0) == 4);
    CHECK(visited(chunk, 0, true) == std::vector<std::int32_t>{2, 0});
    // A vein block is solid to the runs all the same.
    CHECK(chunk.depthBelow(0, 2, 0) == 3);
    // With veins off nothing is a vein block, so it is visited.
    CHECK(visited(chunk, 0, false) == std::vector<std::int32_t>{2, 1, 0});
}

TEST_CASE("a fluid a tick spread reads as the air it flowed into", "[legacy][goldens][decoder]") {
    // The first pass writes only sources; a saved region also holds what its
    // fluid ticks spread, which only ever entered air.
    const NoiseSettings overworld = dimension(kStone, "minecraft:water", true);
    const GoldenChunk chunk = chunkOf(
        {{kStone, kStone, "minecraft:water[level=1]"}, {kStone, "minecraft:lava[level=8]", kStone}},
        overworld);
    CHECK(chunk.categoryAt(0, 2, 0) == Category::Air);
    CHECK(chunk.scanFrom(0, 0) == 1);
    CHECK_FALSE(chunk.waterHeightLow(0, 0).has_value());
    CHECK(visited(chunk, 0, true) == std::vector<std::int32_t>{1, 0});
    // Under solid it still is air: the top-down run restarts below it.
    CHECK(chunk.categoryAt(1, 1, 0) == Category::Air);
    CHECK(chunk.depthAbove(1, 0, 0) == 1);
    CHECK(chunk.depthBelow(1, 2, 0) == 1);
}

TEST_CASE("over the Nether's lava, the bottom-up run starts again", "[legacy][goldens][decoder]") {
    // Lava is the Nether's default fluid, so the old decoder already called
    // it fluid — and then held the bottom-up run through it. A shelf of
    // netherrack over the lava ocean reads 1 on its underside, not 4.
    const NoiseSettings nether = dimension("minecraft:netherrack", "minecraft:lava", false);
    const std::string rack = "minecraft:netherrack";
    const GoldenChunk chunk =
        chunkOf({{rack, rack, rack, kLava, kLava, kLava, rack, rack}}, nether);
    CHECK(chunk.categoryAt(0, 3, 0) == Category::Fluid);
    CHECK(chunk.depthBelow(0, 6, 0) == 1);
    CHECK(chunk.depthBelow(0, 7, 0) == 2);
    CHECK(chunk.depthAbove(0, 2, 0) == 3);
    CHECK(visited(chunk, 0, false) == std::vector<std::int32_t>{7, 6, 2, 1, 0});
}

TEST_CASE("the golden decoder refuses air the first pass never writes",
          "[legacy][goldens][decoder]") {
    const NoiseSettings overworld = dimension(kStone, "minecraft:water", true);
    CHECK_THROWS_AS(chunkOf({{kStone, "minecraft:cave_air"}}, overworld),
                    legacy_goldens::DecodeError);
}
