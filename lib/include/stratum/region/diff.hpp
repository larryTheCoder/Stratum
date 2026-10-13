// Stratum — region comparison, block for block and biome cell for biome cell.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The conformance harness's comparison step (SPEC §7): two region files are
// compared in Java space, before any Bedrock mapping — every block state, and
// every stored biome, by name, at each 4x4x4 cell a section stores one for —
// and the first divergences of each are reported with their coordinates.
// Biomes are reported apart from blocks: a region whose blocks all agree can
// still store the wrong biomes, and before biomes were compared that region
// read as identical.
//
// The rule this file exists to uphold: anything that cannot be compared is
// reported as a difference, never passed over. A chunk that is present on one
// side, or that fails to decode, is a finding — otherwise a run that compared
// almost nothing would look exactly like a run that agreed on everything.

#pragma once

#include <stratum/region/region_file.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stratum::region {

/// Why two chunks could not be compared at all.
enum class ChunkIssue : std::uint8_t {
    OnlyInLeft,
    OnlyInRight,
    Undecodable,
    CoordinateMismatch,
};

[[nodiscard]] std::string_view chunkIssueName(ChunkIssue issue) noexcept;

struct ChunkFinding {
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    ChunkIssue issue = ChunkIssue::OnlyInLeft;
    std::string detail;
};

/// One differing block, in Java block space.
struct BlockDifference {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::string left;
    std::string right;
};

/// One differing biome cell: the 4x4x4 cell whose lowest block is (x, y,
/// z), in Java block space, and the biome each side stores there by name —
/// or "<no biomes>" where that side stores none at that y.
struct BiomeDifference {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::string left;
    std::string right;
};

struct DiffOptions {
    /// How many block differences to record before only counting them.
    /// Reporting the first few with coordinates is what makes a parity
    /// failure debuggable; recording millions is not.
    std::size_t maxBlockDifferences = 16;
    /// The same, for differing biome cells.
    std::size_t maxBiomeDifferences = 16;
};

struct DiffReport {
    std::vector<ChunkFinding> chunkFindings;
    std::vector<BlockDifference> blockDifferences;
    std::vector<BiomeDifference> biomeDifferences;
    std::size_t chunksCompared = 0;
    std::size_t blocksCompared = 0;
    /// Biome cells compared: 64 for every section's worth of y either side
    /// stores, as blocksCompared counts 4096.
    std::size_t biomesCompared = 0;
    /// Total differing blocks, which may exceed blockDifferences.size().
    std::size_t blockDifferenceCount = 0;
    /// Total differing biome cells, which may exceed biomeDifferences.size().
    std::size_t biomeDifferenceCount = 0;

    /// Identical only when every chunk was compared and its blocks AND its
    /// biomes agree.
    [[nodiscard]] bool identical() const noexcept {
        return chunkFindings.empty() && blockDifferenceCount == 0 && biomeDifferenceCount == 0;
    }
};

/// Compares two regions. Chunks are visited in sector-table order, blocks in
/// y, then z, then x order, and biome cells in the same order a cell at a
/// time, so "the first difference" means the same thing on every machine and
/// every run.
[[nodiscard]] DiffReport diff(const RegionFile& left, const RegionFile& right,
                              const DiffOptions& options = {});

} // namespace stratum::region
