// Stratum — region comparison tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The property that matters most here is negative: nothing that could not be
// compared may pass silently. A run that skipped every chunk would otherwise
// be indistinguishable from a run that found them all identical.

#include "support/region_builder.hpp"

#include <stratum/region/diff.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::region::ChunkIssue;
using stratum::region::diff;
using stratum::region::DiffOptions;
using stratum::region::DiffReport;
using stratum::test::RegionBuilder;
using stratum::test::SectionSpec;
using stratum::test::uniformSection;

/// A chunk of stone at section 0, with @p differing positions swapped to
/// dirt.
[[nodiscard]] SectionSpec stoneSection(const std::vector<std::size_t>& dirtAt = {}) {
    SectionSpec section;
    section.y = 0;
    section.palette = {"minecraft:stone", "minecraft:dirt"};
    section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    for (const std::size_t index : dirtAt) {
        section.blocks[index] = 1;
    }
    return section;
}

/// @p section with a biome palette and one index per 4x4x4 cell (y, then z,
/// then x); no indices means a single-entry palette, as vanilla writes a
/// uniform section.
[[nodiscard]] SectionSpec withBiomes(SectionSpec section, std::vector<std::string> palette,
                                     std::vector<std::uint16_t> cells = {}) {
    section.biomePalette = std::move(palette);
    section.biomes = std::move(cells);
    return section;
}

} // namespace

TEST_CASE("identical regions compare equal", "[diff]") {
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection()});
    right.addChunk(0, 0, {stoneSection()});

    const DiffReport report = diff(left.build(), right.build());
    CHECK(report.identical());
    CHECK(report.chunksCompared == 1U);
    CHECK(report.blocksCompared == stratum::chunk::kBlocksPerSection);
    CHECK(report.blockDifferenceCount == 0U);
    CHECK(report.chunkFindings.empty());
    // A section's worth of y: its 64 biome cells, stored by neither side.
    CHECK(report.biomesCompared == stratum::chunk::kBiomesPerSection);
    CHECK(report.biomeDifferenceCount == 0U);
}

TEST_CASE("a single differing block is reported with its coordinates", "[diff]") {
    // y=2, z=3, x=4 inside chunk (1, 1), so world (20, 2, 19).
    const std::size_t index = (2U * 256U) + (3U * 16U) + 4U;

    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(1, 1, {stoneSection()});
    right.addChunk(1, 1, {stoneSection({index})});

    const DiffReport report = diff(left.build(), right.build());
    REQUIRE_FALSE(report.identical());
    REQUIRE(report.blockDifferences.size() == 1U);
    CHECK(report.blockDifferenceCount == 1U);

    const stratum::region::BlockDifference& difference = report.blockDifferences.front();
    CHECK(difference.x == 20);
    CHECK(difference.y == 2);
    CHECK(difference.z == 19);
    CHECK(difference.left == "minecraft:stone");
    CHECK(difference.right == "minecraft:dirt");
}

TEST_CASE("recorded differences are capped but still counted in full", "[diff]") {
    std::vector<std::size_t> differing;
    for (std::size_t i = 0; i < 100; ++i) {
        differing.push_back(i);
    }

    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection()});
    right.addChunk(0, 0, {stoneSection(differing)});

    DiffOptions options;
    options.maxBlockDifferences = 5;
    const DiffReport report = diff(left.build(), right.build(), options);

    CHECK(report.blockDifferences.size() == 5U);
    // The count is the truth; the list is only what was worth printing.
    CHECK(report.blockDifferenceCount == 100U);
}

TEST_CASE("differences are found in a stable order", "[diff]") {
    // y ascending, then z, then x — so "the first difference" names the same
    // block on every machine and every run.
    const std::size_t lower = (1U * 256U) + (0U * 16U) + 0U;
    const std::size_t higher = (9U * 256U) + (0U * 16U) + 0U;

    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection()});
    right.addChunk(0, 0, {stoneSection({higher, lower})});

    DiffOptions options;
    options.maxBlockDifferences = 1;
    const DiffReport report = diff(left.build(), right.build(), options);

    REQUIRE(report.blockDifferences.size() == 1U);
    CHECK(report.blockDifferences.front().y == 1);
}

TEST_CASE("a chunk present on only one side is a finding", "[diff]") {
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection()});
    left.addChunk(2, 0, {stoneSection()});
    right.addChunk(0, 0, {stoneSection()});

    const DiffReport report = diff(left.build(), right.build());
    REQUIRE_FALSE(report.identical());
    REQUIRE(report.chunkFindings.size() == 1U);
    CHECK(report.chunkFindings.front().issue == ChunkIssue::OnlyInLeft);
    CHECK(report.chunkFindings.front().chunkX == 2);
    // The chunk they do share is still compared.
    CHECK(report.chunksCompared == 1U);
}

TEST_CASE("a chunk that cannot be decoded is a finding, never a skip", "[diff][malformed]") {
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection()});
    // Valid framing, but the payload is not chunk NBT.
    right.addChunkPayload(0, 0, std::vector<std::byte>(64, std::byte{0x7F}));

    const DiffReport report = diff(left.build(), right.build());
    REQUIRE_FALSE(report.identical());
    REQUIRE(report.chunkFindings.size() == 1U);
    CHECK(report.chunkFindings.front().issue == ChunkIssue::Undecodable);
    CHECK_FALSE(report.chunkFindings.front().detail.empty());
    CHECK(report.chunksCompared == 0U);
}

TEST_CASE("chunks that disagree about where they are is a finding", "[diff]") {
    // Same slot in the sector table, different xPos/zPos inside the NBT.
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection()});

    const stratum::test::NbtWriter mislabelled =
        stratum::test::buildChunkNbt(32, 64, {stoneSection()});
    right.addChunkPayload(0, 0, mislabelled.bytes());

    const DiffReport report = diff(left.build(), right.build());
    REQUIRE(report.chunkFindings.size() == 1U);
    CHECK(report.chunkFindings.front().issue == ChunkIssue::CoordinateMismatch);
}

TEST_CASE("a section on one side only shows up as differing blocks", "[diff]") {
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {stoneSection(), uniformSection(1, "minecraft:dirt")});
    right.addChunk(0, 0, {stoneSection()});

    const DiffReport report = diff(left.build(), right.build());
    REQUIRE_FALSE(report.identical());
    // Every block of the extra section differs.
    CHECK(report.blockDifferenceCount == stratum::chunk::kBlocksPerSection);
    REQUIRE_FALSE(report.blockDifferences.empty());
    CHECK(report.blockDifferences.front().left == "minecraft:dirt");
    CHECK(report.blockDifferences.front().right == "<no section>");
}

TEST_CASE("two empty regions are identical, having compared nothing", "[diff]") {
    const stratum::region::RegionFile empty =
        stratum::region::RegionFile::fromBytes({}, "r.0.0.mca");
    const DiffReport report = diff(empty, empty);
    CHECK(report.identical());
    CHECK(report.chunksCompared == 0U);
    CHECK(report.blocksCompared == 0U);
}

TEST_CASE("a region pair differing only in its biomes is reported different", "[diff][biome]") {
    // Every block the same; one biome cell differs. Cell 24 is y 1, z 2,
    // x 0 of section 0 in chunk (1, 2): its lowest block is world
    // (16, 4, 40). Neither the cell nor the chunk is the same on x and z, so
    // a lookup that swapped them would name (24, 4, 32), or miss the cell.
    std::vector<std::uint16_t> cells(stratum::chunk::kBiomesPerSection, 0);
    cells[24] = 1;

    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(1, 2, {withBiomes(stoneSection(), {"minecraft:the_end"})});
    right.addChunk(1, 2,
                   {withBiomes(stoneSection(), {"minecraft:the_end", "minecraft:plains"}, cells)});

    const DiffReport report = diff(left.build(), right.build());
    CHECK_FALSE(report.identical());
    CHECK(report.blockDifferenceCount == 0U);
    CHECK(report.blockDifferences.empty());
    CHECK(report.biomesCompared == stratum::chunk::kBiomesPerSection);
    CHECK(report.biomeDifferenceCount == 1U);
    REQUIRE(report.biomeDifferences.size() == 1U);
    const stratum::region::BiomeDifference& difference = report.biomeDifferences.front();
    CHECK(difference.x == 16);
    CHECK(difference.y == 4);
    CHECK(difference.z == 40);
    CHECK(difference.left == "minecraft:the_end");
    CHECK(difference.right == "minecraft:plains");
}

TEST_CASE("biome cells below y=0 are found in their own section", "[diff][biome][landmine]") {
    // Sections -4, -1 and 0 on both sides, one cell differing in each of
    // the two negative ones and none in section 0. Truncating division from
    // y to section folds y -12..-1 onto section 0 and y -60..-49 onto
    // section -3, so it would find neither; floorDiv finds both, each named
    // by its lowest block in chunk (2, 1):
    //   section -4, cell 57 (y 3, z 2, x 1): world (36, -52, 24);
    //   section -1, cell 39 (y 2, z 1, x 3): world (44, -8, 20).
    auto stoneAt = [](const int sectionY) {
        SectionSpec section = stoneSection();
        section.y = sectionY;
        return section;
    };
    std::vector<std::uint16_t> deep(stratum::chunk::kBiomesPerSection, 0);
    deep[57] = 1;
    std::vector<std::uint16_t> shallow(stratum::chunk::kBiomesPerSection, 0);
    shallow[39] = 1;

    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(2, 1,
                  {withBiomes(stoneAt(-4), {"minecraft:the_end"}),
                   withBiomes(stoneAt(-1), {"minecraft:the_end"}),
                   withBiomes(stoneAt(0), {"minecraft:the_end"})});
    right.addChunk(2, 1,
                   {withBiomes(stoneAt(-4), {"minecraft:the_end", "minecraft:end_barrens"}, deep),
                    withBiomes(stoneAt(-1), {"minecraft:the_end", "minecraft:plains"}, shallow),
                    withBiomes(stoneAt(0), {"minecraft:the_end"})});

    const DiffReport report = diff(left.build(), right.build());
    CHECK_FALSE(report.identical());
    CHECK(report.blockDifferenceCount == 0U);
    // y -64..15: five sections' worth of cells, the two absent on both
    // sides (-3, -2) included.
    CHECK(report.biomesCompared == 5U * stratum::chunk::kBiomesPerSection);
    CHECK(report.biomeDifferenceCount == 2U);
    REQUIRE(report.biomeDifferences.size() == 2U);
    CHECK(report.biomeDifferences[0].x == 36);
    CHECK(report.biomeDifferences[0].y == -52);
    CHECK(report.biomeDifferences[0].z == 24);
    CHECK(report.biomeDifferences[0].right == "minecraft:end_barrens");
    CHECK(report.biomeDifferences[1].x == 44);
    CHECK(report.biomeDifferences[1].y == -8);
    CHECK(report.biomeDifferences[1].z == 20);
    CHECK(report.biomeDifferences[1].right == "minecraft:plains");
}

TEST_CASE("biomes are compared by name, not by palette index", "[diff][biome]") {
    // The same biome everywhere, under two palettes that index it differently.
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {withBiomes(stoneSection(), {"minecraft:end_midlands"})});
    right.addChunk(0, 0,
                   {withBiomes(stoneSection(), {"minecraft:plains", "minecraft:end_midlands"},
                               std::vector<std::uint16_t>(stratum::chunk::kBiomesPerSection, 1))});

    const DiffReport report = diff(left.build(), right.build());
    CHECK(report.identical());
    CHECK(report.biomesCompared == stratum::chunk::kBiomesPerSection);
}

TEST_CASE("biomes stored on one side only are differences, never skipped", "[diff][biome]") {
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {withBiomes(stoneSection(), {"minecraft:the_end"})});
    right.addChunk(0, 0, {stoneSection()});

    const DiffReport report = diff(left.build(), right.build());
    CHECK_FALSE(report.identical());
    CHECK(report.blockDifferenceCount == 0U);
    CHECK(report.biomeDifferenceCount == stratum::chunk::kBiomesPerSection);
    REQUIRE_FALSE(report.biomeDifferences.empty());
    CHECK(report.biomeDifferences.front().left == "minecraft:the_end");
    CHECK(report.biomeDifferences.front().right == "<no biomes>");
}

TEST_CASE("recorded biome differences are capped, counted in full, and found in a stable order",
          "[diff][biome]") {
    // Every cell differs; the first recorded must be the lowest cell, y then
    // z then x.
    RegionBuilder left;
    RegionBuilder right;
    left.addChunk(0, 0, {withBiomes(stoneSection(), {"minecraft:end_barrens"})});
    right.addChunk(0, 0, {withBiomes(stoneSection(), {"minecraft:small_end_islands"})});

    DiffOptions options;
    options.maxBiomeDifferences = 3;
    const DiffReport report = diff(left.build(), right.build(), options);
    CHECK(report.biomeDifferenceCount == stratum::chunk::kBiomesPerSection);
    REQUIRE(report.biomeDifferences.size() == 3U);
    CHECK(report.biomeDifferences[0].x == 0);
    CHECK(report.biomeDifferences[0].y == 0);
    CHECK(report.biomeDifferences[0].z == 0);
    CHECK(report.biomeDifferences[1].x == 4);
    CHECK(report.biomeDifferences[1].z == 0);
    // The block cap is separate, and nothing about the blocks differs.
    CHECK(report.blockDifferences.empty());
}
