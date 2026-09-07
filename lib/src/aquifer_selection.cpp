// Stratum — which aquifer sources compete for a block.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/selection.hpp>

#include <cstddef>

namespace stratum::aquifer {

std::array<Candidate, kCandidateCount> candidatesFor(const CentreSource& centres,
                                                     const CellIndex home) noexcept {
    std::array<Candidate, kCandidateCount> out{};
    for (std::size_t i = 0; i < kCandidateCount; ++i) {
        const CandidateOffset& offset = kCandidateWindow[i];
        const CellIndex cell{
            .x = home.x + offset.dx, .y = home.y + offset.dy, .z = home.z + offset.dz};
        out[i] = Candidate{.cell = cell, .centre = centres.centreOf(cell.x, cell.y, cell.z)};
    }
    return out;
}

Selection rankCandidates(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                         const std::span<const Candidate> candidates) noexcept {
    Selection out{};
    for (const Candidate& candidate : candidates) {
        const Source source{.cell = candidate.cell,
                            .centre = candidate.centre,
                            .distanceSq = squaredDistanceTo(candidate.centre, x, y, z)};

        // Every comparison is `<=`, never `<`. On equality the incoming
        // candidate takes the rank and the sitting occupant cascades down —
        // which is the whole of Q4.4, and the reason the window's iteration
        // order is part of the algorithm rather than a detail of this loop.
        if (source.distanceSq <= out.ranked[0].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = out.ranked[1];
            out.ranked[1] = out.ranked[0];
            out.ranked[0] = source;
        } else if (source.distanceSq <= out.ranked[1].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = out.ranked[1];
            out.ranked[1] = source;
        } else if (source.distanceSq <= out.ranked[2].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = source;
        } else if (source.distanceSq <= out.ranked[3].distanceSq) {
            out.ranked[3] = source;
        }
    }
    return out;
}

Selection selectSources(const CentreSource& centres, const std::int32_t x, const std::int32_t y,
                        const std::int32_t z) noexcept {
    const std::array<Candidate, kCandidateCount> candidates =
        candidatesFor(centres, cellOf(x, y, z));
    return rankCandidates(x, y, z, candidates);
}

} // namespace stratum::aquifer
