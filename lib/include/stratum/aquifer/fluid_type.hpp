// Stratum — whether an aquifer's fluid is water or lava.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The last of the four router entries to be measured, and for a long time the
// one nobody had touched at all: every probe this project ever built pinned
// `lava` at the constant -1.0, so neither its rule nor the position it is read
// at was known. That is the failure mode this codebase has now named four
// times — a corpus that holds constant the very thing the answer depends on —
// and it was still armed here.
//
// It matters on its own. A correct level with a wrong type still writes the
// wrong block, so this gates the filler's refusal by itself (SPEC §10,
// milestone MA blocker 4).
//
// WHAT IS MEASURED, on the `elava` probe arm across four world seeds — the one
// arm that gives `lava` vanilla's own noise instead of a constant. 3125 cells
// carrying fluid above the global lava sea, each cell one observation rather
// than its thousands of correlated blocks:
//
//   * The rule scores 0.99873 against a 0.94176 null (2 false positives and 2
//     false negatives). It predicts 180 of 182 lava cells.
//   * The `lava` entry is read at CONTRACTED indices with a horizontal pitch
//     of 64 — NOT the 16 that `fluid_level_spread` uses. This is the sharpest
//     result here: pitch 16 scores 0.9424, pitch 32 0.9472 and pitch 128
//     0.9418, every one of them at or BELOW the null. Only 64 beats guessing.
//   * The threshold is 0.3 and it is a peak, not a plateau: 0.25 leaves 78
//     false positives, 0.35 leaves 31 false negatives, 0.30 leaves 2 of each.
//     It is the only numeric constant about the aquifer published anywhere
//     this project is permitted to read, and it is now also measured.
//   * The comparison is on the ABSOLUTE value. Signed scores 0.96896 on the
//     same cells.
//
// STRICTNESS AT 0.3 IS NOW SETTLED (`tools/analysis/aquifer-fluidtype-probe.sh`,
// group A): a `lava` entry driven to a literal constant instead of through
// noise, at a level forced to -32 (deep enough that the ceiling question
// cannot interfere from either end of its own bracket), swept across the two
// doubles adjacent to 0.3 (`math.nextafter`) and 0.3 itself. The transition
// is exact and lands exactly where the code already reads it: `lava == 0.3`
// is water, the very next representable double above it is lava. Strict
// `>`, confirmed to the ULP rather than assumed.
//
// THE LEVEL CEILING IS PINNED at -10 (same tool, group D). An earlier pass
// had only bracketed it to {-10, -9}, because sweeping `fluid_level_spread`
// steps the level in threes and skipped both candidates: it read -11 (LAVA,
// 14150 cells), -10 (LAVA, 68 cells) and -8 (water, 14004 cells), and no
// configuration it tried produced a level of exactly -9 to test against.
//
// Group D reaches -9 by the two routes that are NOT mod-3 constrained, and
// the transition sits between -10 and -9 on both, on three seeds, at
// 16384 of 16384 columns per dimension with 0 of the other fluid:
//
//   arm Q, the sea branch (floodedness 0.9 past the 0.8 gate, so the level
//     IS `sea_level`): -12/-11/-10 LAVA, -9/-8/-7 water.
//   arm P, the psl cap (`sea_level` -16, spread 6.0 so the cap binds
//     everywhere): -12/-11/-10 LAVA, -9/-8/-7 water.
//   arm P', the same at `sea_level` -70: -12/-10 LAVA, -9/-8 water.
//
// So the ceiling is an ABSOLUTE constant, which is more than the item asked.
// Three sea levels (63, -16, -70) and two level-producing branches put the
// transition at the same absolute pair. A sea-RELATIVE rule is refuted —
// `L <= sea_level - 73` is indistinguishable from -10 at the shipped sea but
// would have moved arm P's transition to -89 and arm P''s to -143 — and so
// is a lambda-relative one, which arm P' moves lambda to -70 precisely to
// separate.
//
// WHY THE EARLIER SWEEP COULD NOT REACH -10 OR -9, now arithmetic rather
// than the "collapsed to the lava-sea floor for a reason not yet understood"
// this comment used to record. The ladder level is
// `40*floorDiv(centreY,40) + 20 + 3*floorDiv(floor(10*spread),3)`, so for a
// constant spread the offset K is uniform world-wide and the reachable
// levels are a mod-3 lattice pinned to the rung. Reaching -9 needs
// `K = -29 - 40*yi` divisible by three, i.e. `yi = 1 (mod 3)`. The attempt
// that collapsed picked `yi = 1` — base 60, cells centred in y in [40,80),
// a whole rung ABOVE the observable band: its -9 sources own no block below
// their own level and place nothing, while every cell that DOES own a block
// in -53..0 lands on base -20 or -60 (levels -89 / -129), below lambda,
// hence dry. What was left in the world was the global lava sea alone.
// The 68-cell -10 reading is the mirror image (base 20, `yi = 0`) and works.
//
// The same arithmetic says the gap is structural, and this is why no golden
// can decide it: with a real spread noise (|s| <= 1) the offset is confined
// to [-12, +9], so the reachable uncapped levels are {-72..-51} u {-32..-11}
// u {8..29} u ... and -10/-9 fall in the hole between -11 and +8. The
// conformance corpus therefore contains no source at either level, and the
// reading above pins the CODE's comparison boundary rather than anything an
// ordinary world produces — group D's spread 6.0 and floodedness 0.9 are far
// outside any real noise, deliberately and as the rest of this corpus does.
//
// ONE READOUT CAVEAT, measured rather than assumed. With the analyzer's old
// ">=5% of bodies" print filter gone, small-count entries appear that are
// NOT source levels: `r_b09` prints `-10:water(w1914/l0)` beside its
// `-11:LAVA(w0/l14150)`. A column dump resolves it — those columns are a
// SINGLE water block at y=-11 over obsidian at -12 over a deep lava body
// (-13..-42), i.e. a water/lava contact film where two territories abut, not
// a source holding level -10. Only the near-16384 entries are readings.
//
// WHAT IS STILL NOT MEASURED, and is marked rather than guessed:
//
//   * Whether a source already reading lava is exempt. Those cells sit below
//     the global lava sea, where nothing can be observed, so the conjunct is
//     carried on the spec's word alone.
//   * Whether a DRY source is exempt (the spec's `L != never` conjunct).
//     REPRESENTABLE NOW, and carried in the predicate below — but as spec
//     hygiene, NOT as a measurement, and the difference is the whole point.
//     Until the dry sentinel landed, `cellFluidLevel` reported a dry source
//     as `level = lambda`, the same number a wet source clamped there got,
//     so the conjunct could not even be written down; it now can, because a
//     dry source reports `kNeverLevel`.
//
//     It is also PROVABLY INERT, which is a stronger statement than
//     "unmeasured" and is why writing it down costs nothing. A source at
//     `kNeverLevel` reads fluid at `y` only if `y < -32512`, which no world
//     has. Every consumer of a source's TYPE is guarded by a reading:
//     `waterOverLava` requires `y < nearestLevel` (substance.hpp),
//     `termFires`' mixed-type constant requires BOTH sources reading fluid
//     (aquifer_barrier.cpp), and `computeSubstance`'s final `Fluid` return
//     requires `nearestReadsFluid`. So no block's output can depend on how a
//     dry source is typed, and the conjunct changes nothing this project can
//     observe — which is exactly what the corpus says: `lava` is a constant
//     0.0 on both worlds Π was measured on, short-circuiting the level term
//     before it is reached, and the `comb_*` worlds pin floodedness at 0.5,
//     so no source is ever dry there either. Calling this "measured" would
//     repeat the error Q4.8 records.
//   * Any `default_fluid` other than water.
#pragma once

#include <stratum/aquifer/lattice.hpp>

#include <cstdint>

namespace stratum::aquifer {

/// The horizontal pitch of the lattice `lava` is addressed on. Distinct from
/// `kCellPitchX` and from the spread's 16, and the distinctness is the
/// measurement: 16, 32 and 128 all score at or below the null on the same
/// cells that give 64 its 0.99873.
inline constexpr std::int32_t kLavaIndexPitchXZ = 64;

/// The vertical pitch, which IS the same 40 the spread and the ladder use.
/// Spelled separately because sharing a number is not the same as sharing a
/// reason, and this one was measured on its own — 20 and 80 both lose.
inline constexpr std::int32_t kLavaIndexPitchY = kBasePitch;

/// How far the `lava` value must sit from zero for a source to turn to lava.
/// STRICT, and confirmed to the ULP: a `lava` driven to exactly 0.3 is
/// water, the next representable double above it is lava
/// (`aquifer-fluidtype-probe.sh` group A — see this file's own header).
inline constexpr double kLavaThreshold = 0.3;

/// How low a source's fluid level must be before the lava override is even
/// considered. PINNED, and inclusive: a source at level -10 is lava, one at
/// -9 is water. Measured on three independent routes to the level (the sea
/// branch, and the psl cap at two different sea levels) across three seeds,
/// every dimension 16384 of 16384 columns with 0 of the other fluid
/// (`aquifer-fluidtype-probe.sh --group d`). The same run shows the ceiling
/// is ABSOLUTE, not sea- or lambda-relative — see this file's own header.
inline constexpr std::int32_t kLavaLevelCeiling = -10;

/// Which fluid a source holds. `Air` is not a case: this answers "given that
/// this source places fluid, which one", and whether it places any at all is
/// `cellFluidLevel`'s question.
enum class FluidType : std::uint8_t {
    /// The dimension's `default_fluid`, whatever that is. Only water has been
    /// observed in this position.
    Default,
    Lava,
};

/// Everything the type decision reads, for ONE source.
struct FluidTypeAt {
    /// The source's own centre y — the same jittered centre the level and the
    /// floodedness are taken at.
    std::int32_t centreY = 0;

    /// The source's fluid level, from `cellFluidLevel`.
    std::int32_t level = 0;

    /// The dimension's `sea_level`, for the global picker's boundary.
    std::int32_t seaLevel = 0;

    /// The `lava` router entry, evaluated at `lavaSample(centre)` — the
    /// contracted indices, NOT the block position. See `sampling.hpp`.
    double lava = 0.0;
};

/// The fluid a source places.
///
/// Below `min(-54, sea_level)` the global picker already reads lava and the
/// aquifer's lattice is skipped, so a source centred there is lava whatever
/// its own noise says. That branch is carried from the level rule's own
/// measurement rather than from this corpus, which cannot see below the lava
/// sea at all.
[[nodiscard]] constexpr FluidType fluidTypeOf(const FluidTypeAt& at) noexcept {
    if (at.centreY < lambdaLevel(at.seaLevel)) {
        return FluidType::Lava;
    }
    // Strict, and on the absolute value. `std::abs` is not constexpr for
    // doubles before C++23, and a ternary is exact here where a subtraction
    // would not be.
    const double magnitude = at.lava < 0.0 ? -at.lava : at.lava;
    return (at.level != kNeverLevel && at.level <= kLavaLevelCeiling && magnitude > kLavaThreshold)
               ? FluidType::Lava
               : FluidType::Default;
}

} // namespace stratum::aquifer
