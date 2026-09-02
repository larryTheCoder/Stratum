// Stratum — region comparison tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The property that matters most here is negative: nothing that could not be
// compared may pass silently. A run that skipped every chunk would otherwise
// be indistinguishable from a run that found them all identical.

#include "support/region_builder.hpp"

#include <stratum/region/diff.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
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
