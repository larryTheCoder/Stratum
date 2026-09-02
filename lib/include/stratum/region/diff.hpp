// Stratum — block-for-block region comparison.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The conformance harness's comparison step (SPEC §7): two region files are
// compared block-for-block in Java block space, before any Bedrock mapping,
// and the first divergences are reported with their coordinates.
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

/// Why two chunks could not be compared block-for-block.
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

struct DiffOptions {
    /// How many block differences to record before only counting them.
    /// Reporting the first few with coordinates is what makes a parity
    /// failure debuggable; recording millions is not.
    std::size_t maxBlockDifferences = 16;
};

struct DiffReport {
    std::vector<ChunkFinding> chunkFindings;
    std::vector<BlockDifference> blockDifferences;
    std::size_t chunksCompared = 0;
    std::size_t blocksCompared = 0;
    /// Total differing blocks, which may exceed blockDifferences.size().
    std::size_t blockDifferenceCount = 0;

    [[nodiscard]] bool identical() const noexcept {
        return chunkFindings.empty() && blockDifferenceCount == 0;
    }
};

/// Compares two regions. Chunks are visited in sector-table order and blocks
/// in y, then z, then x order, so "the first difference" means the same thing
/// on every machine and every run.
[[nodiscard]] DiffReport diff(const RegionFile& left, const RegionFile& right,
                              const DiffOptions& options = {});

} // namespace stratum::region
