// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/lattice.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/javamath.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

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
    const PslRead& surface = cell.surface;
    // Every int operation on a psl reading wraps, as Java's int does: the
    // reading itself saturates (`readPreliminarySurface`), and a saturated
    // value minus a centre is exactly where a bare `-` would be undefined.
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, kOceanGateOffset);

    // THE CLEAN-ROOM SPEC'S Q5.3, AS WRITTEN, and measured as written:
    // `aquifer-ties-probe.sh` drives surfaces on which every reading this
    // function carried before (pipeline engine v12) parts from the spec's,
    // and on both seeds the spec's is the server's on every scored block and
    // fluid-update mark, the v12 reading wrong on every one where they part
    // (vanilla_aquifer_ties_test.cpp, SPEC §11 "The surface scan, sample by
    // sample"). Two short-circuits precede the level rule, and neither reads
    // the floodedness or Q5.9's override.
    //
    // (a) A centre more than twenty above the ANCHOR's surface takes the
    // global picker's status at the centre: the sea at or above lambda,
    // A_lava below it. The anchor, not the scan's minimum: v12 read `cap`
    // (carried from a measurement no corpus could part from the anchor), and
    // a source over a low window sample but a high anchor, centred between
    // the two margins, holds the server's other answer — on land (stone at
    // the sea's lid) and over an aborting anchor alike. The margin is exactly
    // 20 (`kNearSurfaceFloorOffset`): on `aquifer-level-probe.sh`'s 255
    // constant surfaces, 556 964 848 blocks from y -51 up, the clause leaves
    // no block wrong, no clause off the ocean branch parts from it on 3 324,
    // margins of 19 and 21 on 5 536 and 2 807, and a threshold on the sea
    // instead of the surface on 480 493 (vanilla_aquifer_level_test.cpp).
    // And lambda, not -54, is the comparand: at sea -70 (lowsea's a_lo)
    // sources centred -64 to -55 take the sea and are dry.
    if (cell.centreY > javamath::wrappingAdd(surface.anchor, kNearSurfaceFloorOffset)) {
        if (cell.centreY < lambda) {
            return CellLevel{.level = kLavaLevel, .origin = LevelOrigin::GlobalLava};
        }
        // Typed the default fluid (`LevelOrigin::NearSurfaceSea`): over an
        // aborting surface at psl -64 and sea -20, with `lava` 0.5, the server
        // holds water on every source such a sea owns
        // (vanilla_aquifer_fluidnear_test.cpp).
        return CellLevel{.level = cell.seaLevel, .origin = LevelOrigin::NearSurfaceSea};
    }

    // (b) Otherwise the FIRST sample in scan order that is submerged (below
    // `sea_level - 8`) and that the centre sits above less four
    // (`kNearSurfaceDepth`) decides: the global picker's status at that
    // sample's adjusted surface — A_lava if the sample is below the abort
    // threshold, the sea if not. The scan's reduction answers "which comes
    // first" without the samples: every sample before the first abort is a
    // clean one and `gate` is their minimum, which fires exactly when one of
    // them does (both conditions bound the sample from above); an aborting
    // anchor leaves no clean sample before it, and `gate` is then the anchor
    // itself, below the threshold.
    //
    // A clean sample that fires gives the sea, whether or not a later sample
    // aborts and wherever the centre sits: v12 refused that sea to an aborted
    // scan below lambda or within twenty of `cap`, and the server holds it on
    // every block where the two part (the ties probe's tb, tbr and tab
    // worlds, 64 913 scored blocks over two seeds).
    // Its type is the default fluid, as for Q5.3(a)'s sea, even for a centre
    // below the lava sea (cf58 of `aquifer-capfloor-probe.sh`, 20 462 blocks
    // of false barrier typed lava).
    const bool cleanFires = surface.gate >= javamath::wrappingSub(lambda, kOceanGateOffset) &&
                            surface.gate < oceanGate &&
                            javamath::wrappingSub(surface.gate, cell.centreY) < kNearSurfaceDepth;
    if (cleanFires) {
        return CellLevel{.level = cell.seaLevel, .origin = LevelOrigin::NearSurfaceSea};
    }
    // Past the first abort a sample can fire only by aborting itself: a clean
    // one there sits above the aborting sample, so the centre cannot clear it
    // where it does not clear the aborting one. So the first sample to fire,
    // if any does, is an aborting one, and some sample fires exactly when the
    // whole window's minimum does. Then A_lava = (-54, lava), Q1.1's
    // literal level whatever `sea_level` is and lava whatever the source's
    // own `lava` reads — and the floodedness-gated sea is never weighed: on
    // `aquifer-nearsurface-probe.sh`'s frozen corpora refusing it scores
    // 0.9992-0.9996 against 0.066-0.089 for ignoring the abort with the
    // anchor below `sea_level - 8`, and 0.9979-0.9987 against 0.645-0.651
    // at or above it, per seed of two, every block the refusal misses being
    // fluid that moved after generating (vanilla_aquifer_nearsurface_test.cpp;
    // the unfrozen corpus's 0.9911-0.9941 and 0.9812-0.9829 were more of
    // that flow). The level:
    //
    //   * at sea 63, through the barrier: over an aborting surface with
    //     vanilla's barrier on (`aquifer-nsfloor-probe.sh`) a floor at the
    //     dry sentinel writes 3 770 blocks of stone the server does not and
    //     the unfloored ladder 7 690, and under Q5.9's override
    //     (`aquifer-ddfloor-probe.sh`) the sentinel 10 577; -54 writes none.
    //   * below a sea of -54, in blocks: at sea -70 such sources hold lava to
    //     y = -55 on every block they govern (lowsea's a_lo, 135 716 blocks;
    //     `aquifer-lowfloor-probe.sh`'s worlds, with the override and
    //     without), where lambda, lambda with lava, -54 with the source's
    //     own type and lambda + 16 are each refuted.
    if (surface.aborted && javamath::wrappingSub(surface.cap, cell.centreY) < kNearSurfaceDepth) {
        return CellLevel{.level = kLavaLevel, .origin = LevelOrigin::GlobalLava};
    }

    // No sample fires: the level rule, on the minimum of all thirteen samples
    // (Q5.4's S_min, `cap`) and the anchor's ocean test. An aborted scan
    // reaches here only from a centre four or more below every sample, which
    // the spec sends to this rule where v12 gave A_lava. Both sit below
    // lambda, so only Π just above lambda can show which; the ties probe's
    // `td` world is the one surface of -63..-75 where any block parts, all
    // on row lambda (SPEC §11).
    //
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
    // the dry sentinel. It reaches neither short-circuit above, which compare
    // no floodedness at all — the spec's override is on the comparands.
    if (cell.deepDark) {
        return CellLevel{.level = kNeverLevel};
    }

    const std::int32_t ladder = ladderLevel(cell.centreY, surface.cap, cell.spread);
    std::int32_t level = kNeverLevel;
    bool tookSea = false;
    // The DEPTH path gates on the ANCHOR while its depth reads the minimum.
    // Since the minimum never exceeds the anchor this is the harder test, so
    // the two separate only where `sea_level - 8` falls between them — a
    // configuration no campaign had until one went looking for it.
    if (surface.anchor < oceanGate) {
        // `depth` is a plain signed subtraction and may be negative; it never
        // divides, so no floorDiv arises. The only division in the whole
        // decision is the floorDiv inside `ladderLevel`.
        const std::int32_t depth = javamath::wrappingSub(surface.cap, cell.centreY);
        // Clamped at zero and only at zero. Without the clamp a cell with
        // floodedness just above 0.4 would turn to lava past depth 61, where
        // the server was observed keeping the ladder out to depth 160.
        const auto reach =
            static_cast<double>(std::max(0, javamath::wrappingSub(kZeroBonusDepth, depth)));

        // Product over divisor, never a pre-divided constant — see the slope
        // constants in the header.
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
        // No depth term off the ocean branch.
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
    // status an aborting sample gives when it fires. A cell that reaches the
    // level rule keeps its unclamped ladder and its sentinel, below lambda or
    // not: the water/lava and deep-floor worlds measured that.
    if (cell.centreY < lambda && tookSea) {
        return CellLevel{.level = kLavaLevel, .origin = LevelOrigin::GlobalLava};
    }
    return CellLevel{.level = level};
}

namespace {

constexpr std::string_view kAquiferSalt = "minecraft:aquifer";

} // namespace

CentreSource::CentreSource(const std::int64_t worldSeed,
                           const density::RandomSource source) noexcept
    : source_(source) {
    switch (source) {
        case density::RandomSource::Xoroshiro:
            base_ = rng::positionalSourceFor(worldSeed, kAquiferSalt).base();
            break;
        case density::RandomSource::Legacy:
            legacySeed_ = rng::legacyPositionalSourceFor(worldSeed, kAquiferSalt).seed();
            break;
    }
}

Jitter CentreSource::jitterOf(const std::int32_t cx, const std::int32_t cy,
                              const std::int32_t cz) const noexcept {
    // The same three draws in the same order from either generator; only the
    // generator and its bounded draw differ (Xoroshiro's multiply-and-shift,
    // java.util.Random's rejection on the remainder).
    if (source_ == density::RandomSource::Legacy) {
        rng::JavaRandom random = rng::LegacyPositionalSource{legacySeed_}.at(cx, cy, cz);
        const std::int32_t jx = random.nextInt(kJitterBoundX);
        const std::int32_t jy = random.nextInt(kJitterBoundY);
        const std::int32_t jz = random.nextInt(kJitterBoundZ);
        return Jitter{.x = jx, .y = jy, .z = jz};
    }
    rng::Xoroshiro128PlusPlus random = rng::PositionalSource{base_}.at(cx, cy, cz);
    const std::int32_t jx = random.nextInt(kJitterBoundX);
    const std::int32_t jy = random.nextInt(kJitterBoundY);
    const std::int32_t jz = random.nextInt(kJitterBoundZ);
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
