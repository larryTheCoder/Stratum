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
// THE MIXED-TYPE BRANCH (Q6.4's first clause, `Π = 2.0` "if one reads lava
// and the other water") IS MEASURED, and the measurement chose between
// three readings of that sentence rather than confirming one. Every
// barrier probe before `aquifer-waterlava-probe.sh` held `lava` at a
// constant on purpose; its `sea_level` -70 arm is the first world where
// lava-typed sources (centred below lambda) compete with water-typed ones
// on rows the lattice owns, and `vanilla_aquifer_waterlava_test.cpp` scores
// it on three seeds, calling THIS predicate twice per block — as typed, and
// with every source retyped water, which is the predicate exactly as it was
// before the branch:
//
//   * "Reads" means what each source READS AT `y`: the constant applies
//     where BOTH read fluid and the fluids differ — a lava body meeting a
//     water body — and a pair that disagrees at `y` (one fluid, one air)
//     takes the level formula WHATEVER its types. Pooled over the rows the
//     lattice owns, the retyped predicate misses 1698 of the server's real
//     barriers in mixed junctions and this one 590 (1240 -> 330 on the rows
//     above the sea alone); every one of the 1108 blocks the constant adds
//     is server stone; neither predicate writes a block of stone the server
//     does not (0 / 0). Where no pair is mixed the two are the same
//     function (293 / 293 misses, 0 false).
//   * Comparing the two TYPE FIELDS behind the disagree guard — the reading
//     a first implementation here took — makes every row WORSE: on the
//     nearest pair, where the constant alone would fire, the server has
//     stone on 0 of 33 blocks; where the formula fires and the constant
//     would not, on 252 of 252. Refuted.
//   * The types differing REGARDLESS of readings — stone between two
//     DRAINED cells of different type — fills blocks the server leaves
//     open on 99.5-100% of them (0-4 stone of 424-1145 per row). Refuted.
//
// The 590 that remained were never a type question, and they are now
// SPENT: they were `cellFluidLevel`'s contract, not this predicate's. That
// build reported a dry source as `level = lambda` where the clean-room
// spec's is the sentinel `never`, and clamped a ladder that fell below
// lambda back up to it; on the rows just above the sea that puts a plane
// right under the block which the spec does not have, on the `h <= 0` side
// of Π where the divisors are 3/10 instead of 1.5/2.5. Both halves landed
// together (lattice.hpp's `kNeverLevel`), and the sweep that settled them
// is Π's, because no block readout can see a level below lambda at all.
//
// Re-scored over rows lambda-1..+40 on all three water/lava seeds, against
// 88 949 server stone blocks, mixed/pure real-barrier misses run:
//
//     dry=lambda, ladder clamped (the old build)   704 / 1790
//     dry=never only                                33 /  758
//     ladder unclamped only                        677 / 1063
//     both                                           7 /   54
//
// so both halves are load-bearing and neither alone reaches the pair. No
// model in the sweep writes a single block of stone the server does not (0
// false stone throughout), and the same change on the independent
// `barrier3way` world takes the three-source miss count from 121 of 11 923
// real barriers (1.015%) to 6 (0.050%), its mismatching blocks a strict
// SUBSET of the old build's — 115 fixed, none introduced, over 15 728 640
// blocks.
//
// The residual is 7 mixed + 54 pure over that 42-row band, of which 4 + 35
// sit on row lambda itself where Q6.3, Q2.4 and the trailing guard all
// interact, plus the 6 on `barrier3way`. Recorded, not tuned away.
//
// (Those 6 are now CLOSED — they were the agree-guard below, and every one
// of them is decided by the `/2.5` arm it made unreachable. Scored over
// whole worlds rather than that 42-row band, the four pre-existing worlds
// go from 161 real-barrier misses of 243 887 to 39, with no block of false
// stone added anywhere. `barrier3way` itself goes 6 -> 0.)
//
// THE FOURTH DIVISOR (10, for `3 + t <= 0`) IS NOW MEASURED, and the thing
// that had kept it at zero uses was never the worlds — it was a guard in
// THIS build's own `termFires`, `aFluid == bFluid -> return false`, which
// has no counterpart in Q6.4 or Q6.6 and which no probe could ever reach
// past. On the domain that guard admits, `t >= 0.5` for every integer
// `(L_A, L_B, y)` — a pair that disagrees at `y` has `min(L) <= y < max(L)`,
// hence `|h| <= r - 0.5` — so `3 + t >= 3.5 > 0` always and the `/10` arm
// could not fire for ANY input. An exhaustive sweep says the same: of
// 3 135 020 disagreeing combinations (levels -80..129 plus the `never`
// sentinel, y -90..139), `/1.5` takes 1 580 530 and `/3` takes 1 554 490,
// while `/2.5` and `/10` take 0 each. Both were dead code, by arithmetic
// rather than by accident of the corpus.
//
// THE SERVER REFUTES THE GUARD. `tools/analysis/aquifer-deepfloor-probe.sh`
// pins `fluid_level_floodedness` to a constant 0.6 — strictly between
// `kFloodedLocalThreshold` and `kFloodedSeaThreshold`, so every cell takes
// the LADDER instead of `sea_level` and neighbouring cells hold DIFFERENT
// levels, which is the one thing the older worlds could not produce (there
// nearly every flooded cell takes the sea, `Δ = 0`, and no arm is entered
// at all). Over three seeds and seven dimensions, 110 097 250 blocks
// against 4 982 316 blocks of server stone:
//
//     guarded, as shipped before this      480 354 misses   0 false stone
//     guard lifted for both-air only       457 970 misses   0 false stone
//     guard lifted for both-fluid only      22 384 misses   0 false stone
//     no guard at all (Q6.4 as written)          0 misses   0 false stone
//
// Exact, on every block of every arm. The control dimension — real
// floodedness, nothing rescaled — reproduces `barrier3way` exactly (6
// guarded misses, 0 un-gated, 0 false stone either way), so the recipe
// distorts nothing but the level diversity it was built to create.
//
// AND THE DIVISOR ITSELF IS 10, bracketed two-sided. The `/10` arm decides
// 96 292 blocks across the three seeds; on the 65 231 where divisor 10 and
// divisor 3 give different output the server has stone on 65 231 of 65 231,
// so 10 is right on all of them and 3 on none. Perturbing only that divisor,
// pooled misses / false stone against the same 4 982 316:
//
//     1.5  81856/0   2.5  70317/0   3  65231/0   5  45584/0   8  17787/0
//     9  8868/0   9.5  4497/0   9.9  937/0   [10  0/0]   10.1  0/872
//     10.5  0/4410   11  0/8627   12  0/17177   20  0/79432
//
// monotone and one-sided on each side of 10, which pins it to within 1%.
// The `/2.5` arm, dead under the same guard, brackets the same way and
// confirms 2.5: 2.4 leaves 1104 misses, 2.5 is exact, 2.6 writes 1111
// blocks of false stone. That bracket is corroborated on a world NOT built
// for it — on `barrier3way`, 1.5 and 2.0 leave 5 and 1 misses, 2.4/2.5/2.6
// are all exact, 3.0 writes 3 blocks of false stone and 10 writes 37 — so
// `/2.5` is not an artefact of the deepfloor recipe. Both arms mean
// something now that they can be stated: `/2.5` is the barrier's LID a few
// blocks above the higher of two levels, `/10` its FLOOR four or more
// blocks below the lower.
//
// WHAT THE GUARD COST ELSEWHERE: 425 of the 617-block golden-fill residual
// (`golden_fill_aquifer_test.cpp`), which SPEC §11 carried as unattributed
// after the level defects were refuted as its cause. The un-gated residual
// is a strict SUBSET — 6291264 of 6291456 against 6290839, 425 fixed and 0
// introduced — and the 425 are barriers the guard suppressed (289 water ->
// stone, 113 water -> deepslate, 17 water -> dirt, 6 air -> deepslate). The
// 192 that remain are all "we say air, the server says water": a fluid
// EXTENT question, not a barrier one, and the next pass at that residual
// should start there rather than here.
//
// Q6.3's water-over-lava exception is NOT this
// predicate's to implement and no longer a gap: it sits in front of it, in
// substance.hpp's `computeSubstance`, measured on the server (0 of 3320
// blocks it applies to are stone; the bare predicate alone would have
// written 134 of them, and 373 now that the constant is in it — the
// exception's precedence over Q6.6 holds against the new term too).
#pragma once

#include <stratum/aquifer/fluid_type.hpp>

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

/// Q6.4's pressure where two sources BOTH read fluid at the block and the
/// two fluids differ — a lava body against a water body: a constant, with
/// no level arithmetic and no `barrier` noise behind it. At `D = -1` it
/// fires exactly when the pair's weight exceeds 0.5 — a squared-distance
/// separation under 12.5 — whatever the two levels are and whatever the
/// router says.
inline constexpr double kMixedTypePressure = 2.0;

/// One of the three sources the barrier predicate weighs against a block,
/// ranked by distance (selection.hpp: `Selection::ranked[0..2]`). The
/// clean-room spec's status `A = (L, T)`, plus the distance the similarity
/// weighs it by.
struct BarrierSource {
    /// The source's fluid level, from `cellFluidLevel`. A source's fluid
    /// occupies `y < level`.
    std::int32_t level = 0;

    /// The squared euclidean distance from the block's INTEGER position to
    /// this source's jittered centre.
    std::int64_t distanceSq = 0;

    /// The source's fluid TYPE, from `fluidTypeOf`. Read only where the
    /// source reads fluid at the block's own `y` (Q6.4's first clause is
    /// about what each source READS there — this file's header has the
    /// measurement), but carried for every ranked source because which one
    /// that is depends on the block.
    FluidType type = FluidType::Default;
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
/// no barrier evaluation at all) before any term is tried. What a term
/// does then depends on what its pair READS at `y`: one fluid and one air
/// takes the level formula, whatever the two types; both fluid of
/// DIFFERENT types takes the constant `kMixedTypePressure`; both air, or
/// both the same fluid, does not fire at all. Each of those is measured
/// separately against the server (this file's own header).
[[nodiscard]] bool placesBarrier(const BarrierAt& at) noexcept;

} // namespace stratum::aquifer
