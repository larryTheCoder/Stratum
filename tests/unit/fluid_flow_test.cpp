// Stratum — the conformance suite's flow classifier, held without fixtures.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `tests/support/fluid_flow.hpp` decides which golden disagreements are fluid
// that moved after generation, and the golden cases trust it to let nothing
// else through. Those cases need Mojang-derived fixtures CI never has, so the
// classifier's own rules are pinned here on a synthetic region instead: every
// shape it accepts, and the near misses it must not.
#include "support/chunk_builder.hpp"
#include "support/fluid_flow.hpp"
#include "support/region_builder.hpp"
#include "support/temp_path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using stratum::test::Category;
using stratum::test::categoryOf;
using stratum::test::explainedByFlow;
using stratum::test::GoldenRegion;

TEST_CASE("the flow classifier reads a block's exact name", "[test-support]") {
    // The substring classifier this replaced called these two non-solid.
    CHECK(categoryOf("minecraft:oak_stairs") == Category::Solid);
    CHECK(categoryOf("minecraft:water_cauldron") == Category::Solid);
    CHECK(categoryOf("minecraft:lava_cauldron") == Category::Solid);
    CHECK(categoryOf("minecraft:air") == Category::Air);
    CHECK(categoryOf("minecraft:cave_air") == Category::Air);
    CHECK(categoryOf("minecraft:void_air") == Category::Air);
    CHECK(categoryOf("minecraft:water") == Category::Water);
    CHECK(categoryOf("minecraft:lava") == Category::Lava);
}

TEST_CASE("the flow classifier accepts what fluid leaves behind, and nothing else",
          "[test-support]") {
    // One section, y 0..15, of chunk (0, 0); everything at y = 5.
    const std::vector<std::string> palette{"minecraft:air",
                                           "minecraft:stone",
                                           "minecraft:water[level=0]",
                                           "minecraft:water[level=1]",
                                           "minecraft:lava[level=0]",
                                           "minecraft:obsidian",
                                           "minecraft:lava[level=8]"};
    constexpr std::uint16_t kAir = 0;
    constexpr std::uint16_t kStone = 1;
    constexpr std::uint16_t kWater = 2;
    constexpr std::uint16_t kFlowing = 3;
    constexpr std::uint16_t kLava = 4;
    constexpr std::uint16_t kObsidian = 5;
    constexpr std::uint16_t kFallingLava = 6;
    std::vector<std::uint16_t> blocks(4096, kAir);
    const auto put = [&blocks](int x, int y, int z, std::uint16_t block) {
        blocks[static_cast<std::size_t>((((y * 16) + z) * 16) + x)] = block;
    };
    // Flowing water.
    put(2, 5, 2, kFlowing);
    // A water source between two others: the infinite-source rule's shape.
    for (const int x : {4, 5, 6}) {
        put(x, 5, 5, kWater);
    }
    // A water source with one source beside it, which no rule rebuilds.
    put(9, 5, 9, kWater);
    put(10, 5, 9, kWater);
    // Water met lava: obsidian, and water beside it.
    put(12, 5, 12, kObsidian);
    put(12, 5, 13, kWater);
    // A plain stone.
    put(2, 5, 12, kStone);
    // Lava fell onto water: stone, falling lava above, obsidian below.
    put(8, 5, 12, kStone);
    put(8, 6, 12, kFallingLava);
    put(8, 4, 12, kObsidian);
    // Stone under falling lava with nothing of water's beside it.
    put(14, 5, 12, kStone);
    put(14, 6, 12, kFallingLava);
    // A lava source between two others.
    for (const int x : {13, 14, 15}) {
        put(x, 5, 6, kLava);
    }

    stratum::test::RegionBuilder builder;
    builder.addChunk(0, 0, {stratum::test::SectionSpec{0, palette, blocks, {}, {}}});
    const std::filesystem::path path = stratum::test::tempPath("stratum-fluid-flow", ".mca");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(builder.bytes().data()),
                  static_cast<std::streamsize>(builder.bytes().size()));
    }
    GoldenRegion golden(path);
    const auto explained = [&golden](int x, int z, Category goldenCategory, Category raw) {
        return explainedByFlow(golden, x, 5, z, goldenCategory, raw);
    };

    // Accepted: flowing water where the first pass has air; a water source
    // between two sources (the infinite-source rule); obsidian where the
    // first pass has lava; water where the first pass has lava, beside it.
    CHECK(explained(2, 2, Category::Water, Category::Air));
    CHECK(explained(5, 5, Category::Water, Category::Air));
    CHECK(explained(12, 12, Category::Solid, Category::Lava));
    CHECK(explained(12, 12, Category::Solid, Category::Air)); // fluid flowed in first
    CHECK(explained(12, 13, Category::Water, Category::Lava));
    CHECK(explained(8, 12, Category::Solid, Category::Air));

    // Refused: a source with ONE source beside it; stone where the first
    // pass has lava (a barrier the aquifer missed is the aquifer's); golden
    // air where the first pass has fluid (fluid never vanishes by flowing);
    // and a LAVA source between two — lava rebuilds no sources.
    CHECK_FALSE(explained(9, 9, Category::Water, Category::Air));
    CHECK_FALSE(explained(2, 12, Category::Solid, Category::Lava));
    CHECK_FALSE(explained(2, 12, Category::Solid, Category::Air));
    CHECK_FALSE(explained(14, 12, Category::Solid, Category::Air)); // no water ever met it
    CHECK_FALSE(explained(8, 2, Category::Air, Category::Lava));
    CHECK_FALSE(explained(14, 6, Category::Lava, Category::Air));
    // And water beside nothing that water left behind.
    CHECK_FALSE(explained(10, 9, Category::Water, Category::Lava));

    std::filesystem::remove(path);
}

TEST_CASE("a column surface is flow-made only in the shapes flow leaves", "[test-support]") {
    using stratum::chunk::BlockState;
    using stratum::test::flowMadeSurface;
    const BlockState stone{.name = "minecraft:stone", .properties = {}};
    const BlockState grass{.name = "minecraft:grass_block", .properties = {{"snowy", "false"}}};
    const BlockState obsidian{.name = "minecraft:obsidian", .properties = {}};
    const BlockState cobblestone{.name = "minecraft:cobblestone", .properties = {}};
    const BlockState air{.name = "minecraft:air", .properties = {}};
    const BlockState lavaSource{.name = "minecraft:lava", .properties = {{"level", "0"}}};
    const BlockState lavaFlowing{.name = "minecraft:lava", .properties = {{"level", "3"}}};
    const BlockState waterFlowing{.name = "minecraft:water", .properties = {{"level", "1"}}};

    // What water meeting lava leaves is flow's wherever it surfaces.
    CHECK(flowMadeSurface(&obsidian, &air));
    CHECK(flowMadeSurface(&cobblestone, nullptr));
    // Stone is flow's only under FLOWING lava: lava that fell onto water.
    CHECK(flowMadeSurface(&stone, &lavaFlowing));
    CHECK_FALSE(flowMadeSurface(&stone, &lavaSource));
    CHECK_FALSE(flowMadeSurface(&stone, &air));
    CHECK_FALSE(flowMadeSurface(&stone, &waterFlowing));
    CHECK_FALSE(flowMadeSurface(&stone, nullptr));
    // Nothing else is, whatever lies on it.
    CHECK_FALSE(flowMadeSurface(&grass, &lavaFlowing));
    CHECK_FALSE(flowMadeSurface(nullptr, &lavaFlowing));
}
