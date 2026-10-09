// Stratum — the surface pass's per-column reads, by known answers.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// terrain::SurfaceColumn is what terrain::ChunkFiller and the golden-region
// decoder (tools/analysis/legacy-goldens-surface-decoder.hpp) both count the
// stone-depth runs and the water height with. The rules are SPEC §11's
// measured ones ("Lava in the surface pass's runs"); terrain_filler_test.cpp
// holds them end to end through a filled chunk, and this holds the counting
// itself, column by column, so a second caller cannot get it subtly wrong.

#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/surface_column.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using stratum::settings::BlockState;
using stratum::settings::NoiseSettings;
using stratum::terrain::categorize;
using stratum::terrain::Category;
using stratum::terrain::SurfaceColumn;

[[nodiscard]] BlockState state(const std::string& name, const std::string& level = "") {
    BlockState block{.name = stratum::data::ResourceLocation::parse(name), .properties = {}};
    if (!level.empty()) {
        block.properties.emplace("level", level);
    }
    return block;
}

[[nodiscard]] NoiseSettings dimension(const BlockState& defaultBlock,
                                      const BlockState& defaultFluid) {
    NoiseSettings settings;
    settings.defaultBlock = defaultBlock;
    settings.defaultFluid = defaultFluid;
    settings.geometry.minY = -8;
    settings.geometry.height = 24;
    return settings;
}

/// A column written top to bottom, the way one reads it off a diagram: 'S'
/// solid, 'F' fluid, '.' air. Returned bottom to top, as `read` wants it.
[[nodiscard]] std::vector<Category> column(const std::string& topDown) {
    std::vector<Category> categories;
    for (auto it = topDown.rbegin(); it != topDown.rend(); ++it) {
        categories.push_back(*it == 'S' ? Category::Solid
                                        : (*it == 'F' ? Category::Fluid : Category::Air));
    }
    return categories;
}

} // namespace

TEST_CASE("the surface pass's fluid is default_fluid and level-0 lava, in any dimension",
          "[terrain][surface][column]") {
    const NoiseSettings overworld =
        dimension(state("minecraft:stone"), state("minecraft:water", "0"));
    CHECK(categorize(state("minecraft:air"), overworld) == Category::Air);
    CHECK(categorize(state("minecraft:water", "0"), overworld) == Category::Fluid);
    // The aquifer's literal lava, which is not this dimension's default fluid
    // (SPEC §11, measured: fluid to the surface pass exactly as water is).
    CHECK(categorize(state("minecraft:lava", "0"), overworld) == Category::Fluid);
    CHECK(categorize(state("minecraft:stone"), overworld) == Category::Solid);
    CHECK(categorize(state("minecraft:deepslate_iron_ore"), overworld) == Category::Solid);
    // Whole states, not names: the first pass writes no fluid at any other
    // level, and a reader of a saved world undoes flow before it asks.
    CHECK(categorize(state("minecraft:water", "1"), overworld) == Category::Solid);
    CHECK(categorize(state("minecraft:lava", "8"), overworld) == Category::Solid);

    const NoiseSettings nether =
        dimension(state("minecraft:netherrack"), state("minecraft:lava", "0"));
    CHECK(categorize(state("minecraft:lava", "0"), nether) == Category::Fluid);
    CHECK(categorize(state("minecraft:netherrack"), nether) == Category::Solid);
    CHECK(categorize(state("minecraft:air"), nether) == Category::Air);
}

TEST_CASE("top down, air resets the run, solid counts it, fluid holds it",
          "[terrain][surface][column]") {
    // y 3 .. -5: two air, two solid, two fluid, three solid. The fluid neither
    // counts nor breaks the run, so the solid under it carries on from 2.
    SurfaceColumn reads;
    reads.read(column("..SSFFSSS"), -5);
    CHECK(reads.stoneDepthAbove(3) == 0);
    CHECK(reads.stoneDepthAbove(2) == 0);
    CHECK(reads.stoneDepthAbove(1) == 1);
    CHECK(reads.stoneDepthAbove(0) == 2);
    CHECK(reads.stoneDepthAbove(-1) == 2);
    CHECK(reads.stoneDepthAbove(-2) == 2);
    CHECK(reads.stoneDepthAbove(-3) == 3);
    CHECK(reads.stoneDepthAbove(-5) == 5);
    // Air under solid restarts it.
    reads.read(column("SS.S"), 0);
    CHECK(reads.stoneDepthAbove(3) == 1);
    CHECK(reads.stoneDepthAbove(2) == 2);
    CHECK(reads.stoneDepthAbove(1) == 0);
    CHECK(reads.stoneDepthAbove(0) == 1);
}

TEST_CASE("bottom up, every non-solid block resets the run, fluid as much as air",
          "[terrain][surface][column]") {
    // NOT the mirror image of the top-down run (SPEC §11, measured on the
    // lava-run probe's water twins as well as its lava): counting up from
    // the floor, the solid over a fluid pool starts again at 1, where a run
    // that held through the fluid would read 4 there.
    SurfaceColumn reads;
    reads.read(column("SSFFSSS"), 0);
    CHECK(reads.stoneDepthBelow(0) == 1);
    CHECK(reads.stoneDepthBelow(2) == 3);
    CHECK(reads.stoneDepthBelow(3) == 0);
    CHECK(reads.stoneDepthBelow(4) == 0);
    CHECK(reads.stoneDepthBelow(5) == 1);
    CHECK(reads.stoneDepthBelow(6) == 2);
    reads.read(column("SS.SS"), 0);
    CHECK(reads.stoneDepthBelow(3) == 1);
    CHECK(reads.stoneDepthBelow(4) == 2);
}

TEST_CASE("the water height latches one above the first fluid met descending",
          "[terrain][surface][column]") {
    SurfaceColumn reads;
    // Two pools: the upper one sets it, and the lower one does not move it.
    reads.read(column("..FSSFFS"), 10);
    REQUIRE(reads.waterHeight().has_value());
    CHECK(reads.waterHeight() == std::optional<std::int32_t>{16});
    // A column with no fluid has none at all — which makes `water` true.
    reads.read(column("..SSS"), 10);
    CHECK_FALSE(reads.waterHeight().has_value());
}

TEST_CASE("the scan starts at the topmost non-air block, fluid included",
          "[terrain][surface][column]") {
    SurfaceColumn reads;
    reads.read(column("...FSS"), -2);
    CHECK(reads.top() == 0);
    reads.read(column(".S.S"), -2);
    CHECK(reads.top() == 0);
    reads.read(column("...."), -2);
    CHECK(reads.top() == -3);
}

TEST_CASE("a column's reads refuse a y outside it, and a reread forgets the last column",
          "[terrain][surface][column]") {
    SurfaceColumn reads;
    reads.read(column("FSS"), 0);
    CHECK_THROWS_AS(reads.stoneDepthAbove(3), std::out_of_range);
    CHECK_THROWS_AS(reads.stoneDepthBelow(-1), std::out_of_range);
    REQUIRE(reads.waterHeight().has_value());
    // Shorter, lower, no fluid: nothing of the first column survives.
    reads.read(column("SS"), -10);
    CHECK_FALSE(reads.waterHeight().has_value());
    CHECK(reads.top() == -9);
    CHECK(reads.stoneDepthAbove(-10) == 2);
    CHECK_THROWS_AS(reads.stoneDepthAbove(-8), std::out_of_range);
}
