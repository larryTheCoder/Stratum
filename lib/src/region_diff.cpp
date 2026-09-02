// Stratum — block-for-block region comparison.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/diff.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::region {

namespace {

constexpr std::string_view kNoSection = "<no section>";

struct LoadedChunk {
    chunk::Chunk chunk;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] LoadedChunk loadChunk(const RegionFile& region, std::int32_t chunkX,
                                    std::int32_t chunkZ) {
    try {
        const std::vector<std::byte> payload = region.readChunk(chunkX, chunkZ);
        const nbt::Document document = nbt::read(payload);
        return LoadedChunk{chunk::Chunk::decode(document.root), {}};
    } catch (const std::exception& error) {
        return LoadedChunk{chunk::Chunk{}, error.what()};
    }
}

[[nodiscard]] std::string describeBlock(const chunk::Chunk& chunk, int localX, int y, int localZ) {
    const chunk::BlockState* state = chunk.blockAt(localX, y, localZ);
    return state == nullptr ? std::string(kNoSection) : state->toString();
}

} // namespace

std::string_view chunkIssueName(ChunkIssue issue) noexcept {
    switch (issue) {
        case ChunkIssue::OnlyInLeft:
            return "only in the left region";
        case ChunkIssue::OnlyInRight:
            return "only in the right region";
        case ChunkIssue::Undecodable:
            return "could not be decoded";
        case ChunkIssue::CoordinateMismatch:
            return "reports different coordinates on each side";
    }
    return "unknown";
}

DiffReport diff(const RegionFile& left, const RegionFile& right, const DiffOptions& options) {
    DiffReport report;

    for (std::int32_t localZ = 0; localZ < kChunksPerAxis; ++localZ) {
        for (std::int32_t localX = 0; localX < kChunksPerAxis; ++localX) {
            const bool inLeft = left.hasChunk(localX, localZ);
            const bool inRight = right.hasChunk(localX, localZ);

            if (!inLeft && !inRight) {
                continue;
            }
            if (inLeft != inRight) {
                report.chunkFindings.push_back(ChunkFinding{
                    localX, localZ, inLeft ? ChunkIssue::OnlyInLeft : ChunkIssue::OnlyInRight, {}});
                continue;
            }

            const LoadedChunk leftChunk = loadChunk(left, localX, localZ);
            const LoadedChunk rightChunk = loadChunk(right, localX, localZ);
            if (!leftChunk.ok() || !rightChunk.ok()) {
                // Reported, never skipped: an unreadable chunk is a finding,
                // not an absence of evidence.
                report.chunkFindings.push_back(ChunkFinding{
                    localX, localZ, ChunkIssue::Undecodable,
                    !leftChunk.ok() ? "left: " + leftChunk.error : "right: " + rightChunk.error});
                continue;
            }

            if (leftChunk.chunk.x() != rightChunk.chunk.x() ||
                leftChunk.chunk.z() != rightChunk.chunk.z()) {
                report.chunkFindings.push_back(ChunkFinding{
                    leftChunk.chunk.x(), leftChunk.chunk.z(), ChunkIssue::CoordinateMismatch,
                    "left says (" + std::to_string(leftChunk.chunk.x()) + ", " +
                        std::to_string(leftChunk.chunk.z()) + "), right says (" +
                        std::to_string(rightChunk.chunk.x()) + ", " +
                        std::to_string(rightChunk.chunk.z()) + ")"});
                continue;
            }

            ++report.chunksCompared;

            // The union of both y ranges: a section present on one side only
            // must show up as a difference rather than going unvisited.
            const int minY = std::min(leftChunk.chunk.minY(), rightChunk.chunk.minY());
            const int maxY = std::max(leftChunk.chunk.maxY(), rightChunk.chunk.maxY());
            const std::int32_t baseX = leftChunk.chunk.x() * chunk::kSectionSize;
            const std::int32_t baseZ = leftChunk.chunk.z() * chunk::kSectionSize;

            for (int y = minY; y <= maxY; ++y) {
                for (int blockZ = 0; blockZ < chunk::kSectionSize; ++blockZ) {
                    for (int blockX = 0; blockX < chunk::kSectionSize; ++blockX) {
                        ++report.blocksCompared;

                        const chunk::BlockState* leftState =
                            leftChunk.chunk.blockAt(blockX, y, blockZ);
                        const chunk::BlockState* rightState =
                            rightChunk.chunk.blockAt(blockX, y, blockZ);

                        if (leftState == nullptr && rightState == nullptr) {
                            continue;
                        }
                        if (leftState != nullptr && rightState != nullptr &&
                            *leftState == *rightState) {
                            continue;
                        }

                        ++report.blockDifferenceCount;
                        if (report.blockDifferences.size() < options.maxBlockDifferences) {
                            report.blockDifferences.push_back(BlockDifference{
                                baseX + blockX, y, baseZ + blockZ,
                                describeBlock(leftChunk.chunk, blockX, y, blockZ),
                                describeBlock(rightChunk.chunk, blockX, y, blockZ)});
                        }
                    }
                }
            }
        }
    }

    return report;
}

} // namespace stratum::region
