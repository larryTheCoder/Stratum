// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/lattice.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <cmath>

namespace stratum::aquifer {

std::int32_t spreadOffset(const double spread) noexcept {
    // Two floors, both toward negative infinity: std::floor for the scaling,
    // then floorDiv for the grouping into threes. Writing the second as `/ 3`
    // would truncate toward zero and put every negative spread one step high.
    const auto scaled = static_cast<std::int32_t>(std::floor(spread * 10.0));
    return 3 * javamath::floorDiv(scaled, 3);
}

std::int32_t fluidLevel(const std::int32_t base, const double spread) noexcept {
    return base + spreadOffset(spread);
}

CellIndex cellOf(const std::int32_t x, const std::int32_t y, const std::int32_t z) noexcept {
    return CellIndex{.x = javamath::floorDiv(x, kCellPitchX),
                     .y = javamath::floorDiv(y, kCellPitchY),
                     .z = javamath::floorDiv(z, kCellPitchZ)};
}

std::int32_t levelBand(const std::int32_t centreY) noexcept {
    // floorDiv, not `/`. A cell centred at y = -8 sits in band -1; truncation
    // puts it in band 0, which moves its ladder by forty blocks and reads the
    // spread from the wrong place.
    return javamath::floorDiv(centreY, kBasePitch);
}

std::int32_t baseLevel(const std::int32_t y, const std::int32_t preliminarySurface) noexcept {
    const std::int32_t onLattice = (kBasePitch * levelBand(y)) + kBasePhase;
    return std::min(onLattice, preliminarySurface);
}

std::int32_t ladderLevel(const std::int32_t centreY, const std::int32_t preliminarySurface,
                         const double spread) noexcept {
    const std::int32_t onLattice = (kBasePitch * levelBand(centreY)) + kBasePhase;
    // The cap goes on AFTER the offset — measured, and the two orders differ
    // wherever a positive spread would lift the ladder through the surface.
    return std::max(kLavaLevel, std::min(onLattice + spreadOffset(spread), preliminarySurface));
}

std::int32_t cellFluidLevel(const CellFluid& cell) noexcept {
    const std::int32_t ladder = ladderLevel(cell.centreY, cell.preliminarySurface, cell.spread);

    std::int32_t level = kLavaLevel;
    if (cell.preliminarySurface < cell.seaLevel - kOceanGateOffset) {
        // `depth` is a plain signed subtraction and may be negative; it never
        // divides, so no floorDiv arises. The only division in the whole
        // decision is the floorDiv inside `ladderLevel`.
        const std::int32_t depth = cell.preliminarySurface - cell.centreY;
        if (depth < kNearSurfaceDepth) {
            // Unconditional, and deliberately a return rather than an
            // assignment: this is the one path the deep-cell guard below does
            // NOT override. Cells centred at -55, -56 and -57 within three
            // blocks of the preliminary surface were observed taking the sea
            // at floodedness -2.0 and at 0.95 alike.
            return cell.seaLevel;
        }

        // Clamped at zero and only at zero. Without the clamp a cell with
        // floodedness just above 0.4 would turn to lava past depth 61, where
        // the server was observed keeping the ladder out to depth 160.
        const double reach =
            std::max(0.0, static_cast<double>(kZeroBonusDepth) - static_cast<double>(depth));
        if (cell.floodedness + (reach * kSeaBonusSlope) > kFloodedSeaThreshold) {
            level = cell.seaLevel;
        } else if (cell.floodedness + (reach * kLocalBonusSlope) > kFloodedLocalThreshold) {
            level = ladder;
        } else {
            level = kLavaLevel;
        }
    } else if (cell.floodedness > kFloodedSeaThreshold) {
        // No depth term and no near-surface rule at all off the ocean branch.
        level = cell.seaLevel;
    } else if (cell.floodedness > kFloodedLocalThreshold) {
        level = ladder;
    }

    // A cell centred below the lava sea never reaches the sea level THROUGH
    // the gates. Measured at psl -54, which is the only place the two
    // candidate placements of this guard can differ.
    //
    // The fallback is the ladder rather than a bare -54. The two are identical
    // in every configuration measured, because the ladder clamps to -54 for
    // these centres at spread 0; they part only when a positive spread offset
    // lifts the clamped ladder above the lava. That case is still open, and it
    // is worth at most one block of aquifer at the world bottom.
    if (cell.centreY < kLavaLevel && level == cell.seaLevel) {
        level = ladder;
    }
    return level;
}

CentreSource::CentreSource(const std::int64_t worldSeed) noexcept
    : base_(rng::positionalSourceFor(worldSeed, "minecraft:aquifer").base()) {}

Jitter CentreSource::jitterOf(const std::int32_t cx, const std::int32_t cy,
                              const std::int32_t cz) const noexcept {
    rng::Xoroshiro128PlusPlus source = rng::PositionalSource{base_}.at(cx, cy, cz);
    const std::int32_t jx = source.nextInt(kJitterBoundX);
    const std::int32_t jy = source.nextInt(kJitterBoundY);
    const std::int32_t jz = source.nextInt(kJitterBoundZ);
    return Jitter{.x = jx, .y = jy, .z = jz};
}

CellIndex CentreSource::centreOf(const std::int32_t cx, const std::int32_t cy,
                                 const std::int32_t cz) const noexcept {
    const Jitter jitter = jitterOf(cx, cy, cz);
    return CellIndex{.x = (cx * kCellPitchX) + jitter.x,
                     .y = (cy * kCellPitchY) + jitter.y,
                     .z = (cz * kCellPitchZ) + jitter.z};
}

} // namespace stratum::aquifer
