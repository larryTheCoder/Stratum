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
    //
    // Java's `(int) Math.floor` and Java's wrapping `*`: a datapack's spread
    // can be NaN (-> 0) or far outside the int range (-> saturated), where a
    // bare cast is undefined, and `3 * floorDiv(INT_MIN, 3)` overflows by one.
    // No vanilla router comes near either; the point is that no input is UB.
    const std::int32_t scaled = javamath::floorToInt(spread * 10.0);
    return javamath::wrappingMul(3, javamath::floorDiv(scaled, 3));
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

std::int32_t ladderLevel(const std::int32_t centreY, const std::int32_t cap,
                         const double spread) noexcept {
    const std::int32_t onLattice = (kBasePitch * levelBand(centreY)) + kBasePhase;
    // The cap goes on AFTER the offset — measured, and the two orders differ
    // wherever a positive spread would lift the ladder through the surface.
    //
    // NO LOWER CLAMP. Q5.7 is a `min` with the surface and nothing else, and
    // this line used to carry a `max(lambdaLevel(seaLevel), ...)` in front of
    // it. What the earlier campaigns measured there was the clamp's VALUE
    // (lambda against a bare -54), which no readout can distinguish from its
    // ABSENCE — see `kNeverLevel` and lattice.hpp's note on this function. Π
    // can, and does: unclamping cuts 704/1790 mixed/pure barrier misses to
    // 677/1063 on its own and to 7/54 with the dry sentinel, on 88 949 server
    // stone blocks over rows lambda-1..+40 of three probe worlds.
    //
    // The result can now sit arbitrarily far below `lambda`, which is the
    // point. `levelPressure` casts both levels to double before any
    // arithmetic and the int32 difference tops out around 32 900 even against
    // `kNeverLevel`, so nothing here can wrap.
    //
    // The `seaLevel` parameter went with the clamp: nothing else in Q5.7
    // reads it, and leaving it would suggest the sea still enters a ladder
    // level, which it does not.
    return std::min(javamath::wrappingAdd(onLattice, spreadOffset(spread)), cap);
}

CellLevel cellLevel(const CellFluid& cell) noexcept {
    const std::int32_t lambda = lambdaLevel(cell.seaLevel);
    const std::int32_t ladder = ladderLevel(cell.centreY, cell.surface.cap, cell.spread);
    // Every int operation on a psl reading wraps, as Java's int does: the
    // reading itself saturates (`readPreliminarySurface`), and a saturated
    // value minus a centre is exactly where a bare `-` would be undefined.
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, kOceanGateOffset);

    // The near-surface path, gated and measured on the scan's PREFIX minimum.
    // It is an early return and it bypasses the trailing guard — two
    // purpose-built campaigns put cells centred below the lava level wet to
    // the top of their territory, where an assignment would have floored them.
    if (cell.surface.gate < oceanGate &&
        javamath::wrappingSub(cell.surface.gate, cell.centreY) < kNearSurfaceDepth) {
        if (!cell.surface.aborted) {
            return CellLevel{.level = cell.seaLevel, .origin = LevelOrigin::NearSurfaceSea};
        }
        // An aborting cell is floored instead, unless it sits clear of the
        // scan's own low sample by more than twenty blocks. Both terms read
        // `cap`; `gate` and `anchor` are right on 0 of 2067 cells — and MA
        // blocker 2's own remaining "still unverified" mark on this term is
        // now closed too: every earlier probe that could abort here held psl
        // CONSTANT, which makes `gate` and `cap` the same number by
        // construction (see sampling.hpp's own note on `readPreliminarySurface`)
        // and so could never separate them from this comparand specifically.
        // `tools/analysis/aquifer-nearsurface-probe.sh` drives a genuinely
        // varying psl instead: read block-by-block rather than by fluid body
        // (a body-boundary reading is corrupted here by water/lava contact
        // turning to obsidian mid-column), `cap` scores a perfect 1.0000
        // against `gate`'s 0.9266-0.9358 on 7.8M+ discriminating blocks
        // across two seeds.
        //
        // The comparand is `lambda`, not the bare `kLavaLevel` this line used
        // to read: at `sea_level` -70 (lowsea's a_lo, psl -85, where the
        // lattice is consulted up to y_skip -50) cells centred from -64 to
        // -55 take the sea and are dry, where a `kLavaLevel` comparand
        // floors them.
        //
        // The sea here is the other short-circuit (spec Q5.3(a)): the global
        // picker's status at the cell's own centre, which sits at or above
        // lambda, so it carries the near-surface origin and is typed the
        // default fluid — over an aborting surface at psl -64 and sea -20,
        // with `lava` 0.5, the server holds water on every source it owns
        // (vanilla_aquifer_fluidnear_test.cpp).
        if (cell.centreY >= lambda &&
            cell.centreY > javamath::wrappingAdd(cell.surface.cap, kNearSurfaceFloorOffset)) {
            return CellLevel{.level = cell.seaLevel, .origin = LevelOrigin::NearSurfaceSea};
        }
        // Otherwise the FLOOR: the global picker's status at the surface the
        // scan met submerged in the lava sea (spec Q5.3(b)) — A_lava, -54
        // and lava, not lambda and not the cell's own type. A centre below
        // lambda more than twenty above the cap is Q5.3(a)'s global status,
        // which there is the same A_lava. At every `sea_level >= -54` the
        // level is lambda's and no consumer can see the type (SPEC §11);
        // below it they part in blocks, and the server holds A_lava: at sea
        // -70 (lowsea's a_lo) every one of 135 716 blocks whose nearest
        // cell takes this floor is lava up to y = -55 or barrier stone, not
        // one air, and at sea -60 too (aquifer-lowfloor-probe.sh's lf_f60,
        // where the literal -54 and lambda + 16 part).
        //
        // AND THIS FLOOR IS NOT `kNeverLevel`, unlike the dry outcome
        // further down — measured, not assumed. No block readout at an
        // ordinary sea can tell them apart (both are dry at every
        // y >= lambda), and every world that reached this branch held the
        // barrier off, so for a while it was a choice. They part in the
        // barrier's pressure term: `aquifer-nsfloor-probe.sh` turns the
        // barrier on over this branch, and on the 1666 blocks where the two
        // floors give different verdicts the server sides with -54 on all
        // 1666 (vanilla_aquifer_nsfloor_test.cpp).
        return CellLevel{.level = kLavaLevel, .origin = LevelOrigin::GlobalLava};
    }

    // An aborted scan off the near-surface path takes the same A_lava, and
    // before the level rule: Q5.3's short-circuits precede it, so neither a
    // floodedness gate nor Q5.9's override (which forces only that rule's
    // comparands) is reached.
    //
    // So the abort refuses the sea outcome outright, which is MA blocker 2's
    // other mark, closed: `aquifer-nearsurface-probe.sh` drives `aborted`
    // true while floodedness alone would cross the depth path's sea gate,
    // and refusing the sea scores 0.9911-0.9941 against 0.0564-0.0924 for
    // ignoring the abort, on 469575-541125 discriminating blocks across two
    // seeds; off the ocean branch, 0.9812-0.9829 against 0.6612-0.6872.
    //
    // And what it takes instead is -54, lava. At sea 63, through the
    // barrier, the level: the unfloored ladder built 7 690 blocks of stone
    // the server does not and the dry sentinel 3 770
    // (aquifer-nsfloor-probe.sh), and the override's sentinel 10 577
    // (aquifer-ddfloor-probe.sh), where -54 builds none — pipeline engines
    // v5 and v6, which read it as lambda, the same number there. At sea -70,
    // in blocks (aquifer-lowfloor-probe.sh): such cells hold lava to y = -55
    // where lambda's reading leaves them dry, with the override and without.
    if (cell.surface.aborted) {
        return CellLevel{.level = kLavaLevel, .origin = LevelOrigin::GlobalLava};
    }

    // The DRY level is the spec's sentinel, not `lambda`. Q2.4 hands every
    // row below `lambda` to the global lava sea before the lattice is
    // consulted, so the two are indistinguishable in any block readout — the
    // four campaigns that recorded this outcome as `lambda` were not wrong,
    // only under-determined. They part company in the barrier's Π, which
    // weighs both levels whether or not either is readable: dropping the dry
    // level to the sentinel cuts 704/1790 mixed/pure misses to 33/758 on its
    // own, and to 7/54 with the unclamped ladder, over 88 949 server stone
    // blocks on rows lambda-1..+40 of three probe worlds — and to 6 of 11 923
    // real barriers, from 121, on the independent `barrier3way` world. No
    // model in that sweep writes one block of stone the server does not.
    // Q5.9, the deep-dark override: both floodedness comparands forced to -1.
    // Neither branch below can then clear its threshold (0.4 and 0.8, with a
    // depth bonus of at most 52 * 3/160 on the ocean branch), so the level is
    // the dry sentinel. It cannot reach the near-surface return above, which
    // compares no floodedness at all — the spec's override is on the
    // comparands, and that branch has none. Nor does it reach an aborted
    // scan, whose status was returned above.
    if (cell.deepDark) {
        return CellLevel{.level = kNeverLevel};
    }

    std::int32_t level = kNeverLevel;
    bool tookSea = false;
    // The DEPTH path gates on the ANCHOR while everything above gates on the
    // minimum. Since the minimum never exceeds the anchor this is the harder
    // test, so the two separate only where `sea_level - 8` falls between them
    // — a configuration no campaign had until one went looking for it.
    if (cell.surface.anchor < oceanGate) {
        // `depth` is a plain signed subtraction and may be negative; it never
        // divides, so no floorDiv arises. The only division in the whole
        // decision is the floorDiv inside `ladderLevel`.
        const std::int32_t depth = javamath::wrappingSub(cell.surface.gate, cell.centreY);
        // Clamped at zero and only at zero. Without the clamp a cell with
        // floodedness just above 0.4 would turn to lava past depth 61, where
        // the server was observed keeping the ladder out to depth 160.
        const auto reach =
            static_cast<double>(std::max(0, javamath::wrappingSub(kZeroBonusDepth, depth)));

        // Product over divisor, never a pre-divided constant — see the slope
        // constants in the header. No abort reaches here (above).
        if (cell.floodedness + ((reach * kSeaBonusNumerator) / kSeaBonusDenominator) >
            kFloodedSeaThreshold) {
            level = cell.seaLevel;
            tookSea = true;
        } else if (cell.floodedness + ((reach * kLocalBonusNumerator) / kLocalBonusDenominator) >
                   kFloodedLocalThreshold) {
            level = ladder;
        } else {
            level = kNeverLevel;
        }
    } else if (cell.floodedness > kFloodedSeaThreshold) {
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
    //
    // It is Q5.6's `Global(Q)` for a centre below lambda: A_lava, the same
    // status an aborted scan takes. A cell that did not abort keeps its
    // unclamped ladder and its sentinel, below lambda or not: the water/lava
    // and deep-floor worlds measured that.
    if (cell.centreY < lambda && tookSea) {
        return CellLevel{.level = kLavaLevel, .origin = LevelOrigin::GlobalLava};
    }
    return CellLevel{.level = level};
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
