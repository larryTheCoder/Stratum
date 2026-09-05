// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/barrier.hpp>

#include <algorithm>

namespace stratum::aquifer {

bool placesBarrier(const BarrierAt& at) noexcept {
    // The pair must disagree: if both sources put the same thing here there is
    // nothing to partition.
    if ((at.y < at.levelA) == (at.y < at.levelB)) {
        return false;
    }
    if (at.separation >= kSimilarityRange) {
        return false;
    }

    const std::int32_t lower = std::min(at.levelA, at.levelB);
    const std::int32_t upper = std::max(at.levelA, at.levelB);

    // How far the block sits above the air source's level, and below the fluid
    // source's. Equivalently the block centre y + 0.5 is `above + 0.5` from
    // the lower plane and `below - 0.5` from the upper one; the rule takes
    // whichever is nearer, and a tie — which happens only for odd gaps, at
    // below == above + 1 — goes to the LOWER plane.
    const std::int32_t above = at.y - lower;
    const std::int32_t below = upper - at.y;
    const bool lowerIsNearer = above < below;

    const std::int32_t pressure = lowerIsNearer ? ((2 * above) + 7) : ((4 * below) - 2);
    const bool routerApplies =
        lowerIsNearer ? (above <= kBarrierReachAbove) : (below <= kBarrierReachBelow);
    const std::int32_t similarity = kSimilarityRange - at.separation;

    if (!routerApplies) {
        // Exact integer arithmetic, which is the whole point of the spelling:
        // outside the router's reach nothing here can round.
        return (similarity * pressure) > kBarrierPressure;
    }
    return (static_cast<double>(similarity) *
            (static_cast<double>(pressure) + (6.0 * at.barrier))) >
           static_cast<double>(kBarrierPressure);
}

} // namespace stratum::aquifer
