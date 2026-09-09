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
    // The shift is part of the mapping, not a separate step, and it moves
    // every cell boundary: the x boundary sits at x = 5 (mod 16) rather than
    // at 0. floorDiv on the SHIFTED coordinate — shifting after a truncating
    // division would be wrong twice over.
    return CellIndex{.x = javamath::floorDiv(x + kCellShiftX, kCellPitchX),
                     .y = javamath::floorDiv(y + kCellShiftY, kCellPitchY),
                     .z = javamath::floorDiv(z + kCellShiftZ, kCellPitchZ)};
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

std::int32_t ladderLevel(const std::int32_t centreY, const std::int32_t cap, const double spread,
                         const std::int32_t seaLevel) noexcept {
    const std::int32_t onLattice = (kBasePitch * levelBand(centreY)) + kBasePhase;
    // The cap goes on AFTER the offset — measured, and the two orders differ
    // wherever a positive spread would lift the ladder through the surface.
    return std::max(lambdaLevel(seaLevel), std::min(onLattice + spreadOffset(spread), cap));
}

std::int32_t cellFluidLevel(const CellFluid& cell) noexcept {
    const std::int32_t lambda = lambdaLevel(cell.seaLevel);
    const std::int32_t ladder =
        ladderLevel(cell.centreY, cell.surface.cap, cell.spread, cell.seaLevel);
    const std::int32_t oceanGate = cell.seaLevel - kOceanGateOffset;

    // The near-surface path, gated and measured on the scan's PREFIX minimum.
    // It is an early return and it bypasses the trailing guard — two
    // purpose-built campaigns put cells centred below the lava level wet to
    // the top of their territory, where an assignment would have floored them.
    if (cell.surface.gate < oceanGate && cell.surface.gate - cell.centreY < kNearSurfaceDepth) {
        if (!cell.surface.aborted) {
            return cell.seaLevel;
        }
        // An aborting cell is floored instead, unless it sits clear of the
        // scan's own low sample by more than twenty blocks. Both terms read
        // `cap`; `gate` and `anchor` are right on 0 of 2067 cells.
        //
        // BOTH the comparand and the floor are `lambda`, not the bare
        // `kLavaLevel` this line used to read. Measured at `sea_level` -70,
        // where the two part company: 251229 blocks the old comparand calls
        // wet up to y=-55 are observed dry at every one, 0/251229. The
        // comparand cannot be separated from the floor the same way — lambda
        // equals `sea_level` on every `sea_level < -54` world by definition,
        // so a cell that takes the "true" branch and a cell that takes the
        // "false" branch under a lambda-based comparand are indistinguishable
        // downstream, whatever comparand put them there. That is a PERMANENT
        // TIE, not an open measurement: no world can separate them, and using
        // `lambda` in both places is adopted because it is a no-op at every
        // `sea_level` this project had already verified (lambda equals
        // `kLavaLevel` there), not because the comparand itself was isolated.
        return (cell.centreY >= lambda && cell.centreY > cell.surface.cap + kNearSurfaceFloorOffset)
                   ? cell.seaLevel
                   : lambda;
    }

    std::int32_t level = lambda;
    bool tookSea = false;
    // The DEPTH path gates on the ANCHOR while everything above gates on the
    // minimum. Since the minimum never exceeds the anchor this is the harder
    // test, so the two separate only where `sea_level - 8` falls between them
    // — a configuration no campaign had until one went looking for it.
    if (cell.surface.anchor < oceanGate) {
        // `depth` is a plain signed subtraction and may be negative; it never
        // divides, so no floorDiv arises. The only division in the whole
        // decision is the floorDiv inside `ladderLevel`.
        const std::int32_t depth = cell.surface.gate - cell.centreY;
        // Clamped at zero and only at zero. Without the clamp a cell with
        // floodedness just above 0.4 would turn to lava past depth 61, where
        // the server was observed keeping the ladder out to depth 160.
        const auto reach = static_cast<double>(std::max(0, kZeroBonusDepth - depth));

        // Product over divisor, never a pre-divided constant — see the slope
        // constants in the header. The abort refuses the sea outcome outright.
        if (!cell.surface.aborted &&
            cell.floodedness + ((reach * kSeaBonusNumerator) / kSeaBonusDenominator) >
                kFloodedSeaThreshold) {
            level = cell.seaLevel;
            tookSea = true;
        } else if (cell.floodedness + ((reach * kLocalBonusNumerator) / kLocalBonusDenominator) >
                   kFloodedLocalThreshold) {
            level = ladder;
        } else {
            level = lambda;
        }
    } else if (!cell.surface.aborted && cell.floodedness > kFloodedSeaThreshold) {
        // No depth term and no near-surface rule at all off the ocean branch.
        level = cell.seaLevel;
        tookSea = true;
    } else if (cell.floodedness > kFloodedLocalThreshold) {
        level = ladder;
    }

    // The trailing guard reads NO psl at all — all eight candidate sources tie
    // with zero differing cells. Its threshold is lambda, and its replacement
    // is the LITERAL lava level, which is the one place the two part company:
    // at `sea_level` -56 the guard raises a cell to -54, ABOVE lambda.
    //
    // It tests which BRANCH produced the level, not whether the number equals
    // `sea_level`. Those coincide for every ordinary sea, and part exactly
    // where this was measured: at `sea_level` -56 the third outcome is also
    // -56, and a numeric test would floor it to -54 — which the server does
    // not do. Same world, same cells, floodedness 1.0 reads -54 and 0.3 reads
    // -56, so the branch is the discriminator and the value cannot be.
    if (cell.centreY < lambda && tookSea) {
        level = kLavaLevel;
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
