// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Where two aquifer cells would place different things — one fluid, one air —
// the server writes a sheet of stone between them. SPEC recorded that as
// geometry with an unrecovered threshold "around 23", and that reading was
// wrong in an instructive way: 23 is not a constant of the algorithm, it is
// the value the real rule takes over the commonest geometry.
//
// THREE SOURCES, NOT TWO (MA blocker 3, CLOSED). A two-source-only version of
// this predicate — no density term, no third source — was exact on the pairs
// it could see (251,658,240 blocks across 40 probe dimensions and six world
// seeds) but missed 11-18% of the server's REAL barrier blocks outright,
// because they come from a third source it could not see at all. The
// clean-room spec's Q6.6 (spec/aquifer-spec.md) names the replacement shape:
// three ranked sources, an additive caller density `D`, and a pressure
// function `Π` (Q6.4). Landing it took two steps, not one:
//
//   1. PURE ARITHMETIC, no probe needed. At `D = -1` — every Stratum aquifer
//      probe's own density constant so far — Q6.4's formula with its stated
//      divisors (1.5/2.5 for the near-fluid branch, 3 for the near-air
//      branch) reproduces the OLD, already-confirmed two-source rule EXACTLY
//      over an exhaustive sweep of 39150 (gap, position, separation, barrier)
//      combinations: 0 mismatches. The old rule's own 251M-block validation
//      transitively confirms those two divisors; no new server query needed.
//
//   2. A REAL PROBE for what the sweep in (1) structurally could not reach:
//      a genuine third source. `tools/analysis/aquifer-barrier-probe.sh`
//      drives `barrier`, `fluid_level_floodedness` and `fluid_level_spread`
//      with vanilla's own REAL noises (not a synthetic field — a synthetic
//      field built for one question has no reason to produce the dense,
//      irregular cell-to-cell variation real three-way junctions need), at
//      three density constants and two world seeds. The three-source formula
//      above rescues 83-98% of the two-source rule's errors, cutting the
//      real-barrier miss rate from 11-18% to 0.4-2.4% (13.19%/16.48% down to
//      1.01%/1.02% overall, seed-to-seed) — matching the ~13% figure this
//      project had already measured by an entirely different route. A
//      self-check against the OLD two-source rule at D=-1, on the SAME real
//      block data, landed 0/1,646,376+1,645,918 mismatches across both
//      seeds.
//
// STILL UNMEASURED, and marked rather than guessed: the pressure function's
// fourth divisor (10, for `3 + t <= 0` — the "near-air-far" branch). It is
// not what the residual above comes from — across both barrier-probe seeds
// it was never once reached by a real third-source pair, residual or
// otherwise, so there is no case to measure it against. It is implemented as
// the clean-room spec states, structurally inert until a configuration that
// exercises it is found. Also unmeasured: the mixed-fluid-type branch
// (`Π = 2.0` when one source reads lava and the other water) — this
// project's own barrier probes hold `lava` at a constant specifically to
// keep that question separate, and Q6.3's water-over-lava exception, which
// this predicate does not implement at all yet.
#pragma once

#include <cstdint>

namespace stratum::aquifer {

/// The separation at which two sources stop competing. Beyond it no barrier is
/// written whatever the pressure says, and the clamp is real rather than
/// cosmetic: without it a negative similarity multiplied by a negative
/// pressure produces barriers, which was observed firing 478 times at a
/// `barrier` input of -1.0.
inline constexpr std::int32_t kSimilarityRange = 25;

/// How near the nearer plane a block must sit for the `barrier` router value
/// to enter Π at all — the old two-source rule's own reach, still exact at
/// D=-1 (see this file's own header): thirteen barrier constants from -1.0 to
/// +4.0 give byte-identical output beyond it.
inline constexpr std::int32_t kBarrierReachAbove = 2; ///< on the air side, u <= 2
inline constexpr std::int32_t kBarrierReachBelow = 3; ///< on the fluid side, v <= 3

/// One of the three sources the barrier predicate weighs against a block,
/// ranked by distance (selection.hpp: `Selection::ranked[0..2]`).
struct BarrierSource {
    /// The source's fluid level, from `cellFluidLevel`. A source's fluid
    /// occupies `y < level`.
    std::int32_t level = 0;

    /// The squared euclidean distance from the block's INTEGER position to
    /// this source's jittered centre.
    std::int64_t distanceSq = 0;
};

/// One block, weighed against its three nearest aquifer sources.
struct BarrierAt {
    /// The block's y. The rule is per block, and the `barrier` value below is
    /// read at the block's own position rather than at any source's centre.
    std::int32_t y = 0;

    /// `D`: the caller's own density before the aquifer touches it (Q2.2) —
    /// solid is unconditional at `D > 0`, so the barrier only ever adds solid
    /// on top of a density that would otherwise be non-solid. The old
    /// two-source rule had no such term because it was fit to exactly one
    /// value of it, `D = -1`, which is why it could not generalise.
    double density = 0.0;

    /// The three nearest sources, nearest first.
    BarrierSource nearest;
    BarrierSource second;
    BarrierSource third;

    /// The `barrier` noise-router entry, evaluated at this block's position.
    double barrier = 0.0;
};

/// Whether the server writes stone here.
///
/// `D + s12*Π(A1,A2) > 0`, or (`s13 > 0` and `D + s12*s13*Π(A1,A3) > 0`), or
/// (`s23 > 0` and `D + s12*s23*Π(A2,A3) > 0`) — spec/aquifer-spec.md Q6.6,
/// with `s_ij = 1 - (dj-di)/25` on squared distances and `Π` per Q6.4. `s12
/// <= 0` short-circuits to false outright (Q6.2: the nearest point wins,
/// no barrier evaluation at all) before any term is tried. Each term ALSO
/// requires its own pair to actually disagree at `y` — the only regime this
/// project has validated `Π` against real data in (this file's own header);
/// applying it to a pair that agrees at `y` is unvalidated extrapolation this
/// function deliberately does not make, and such a term simply does not
/// fire.
[[nodiscard]] bool placesBarrier(const BarrierAt& at) noexcept;

} // namespace stratum::aquifer
