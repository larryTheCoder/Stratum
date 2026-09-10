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
// THE LEVEL CEILING IS NARROWED, not yet pinned (same tool, group B/C):
// sweeping `fluid_level_spread` — including past the +-1.0 any real noise
// reaches, the same kind of extreme point the rest of this corpus already
// uses to pin an exact constant — reaches level -11 (LAVA, 14150 cells),
// -10 (LAVA, 68 cells, a rarer configuration but unambiguous: 0 water) and
// -8 (water, 14004 cells). That brackets the true ceiling to {-10, -9}: the
// -10 default below fits every reading and needs no correction to stay
// consistent with them, but -9 has not been RULED OUT — no reachable
// configuration found in this pass produced a level of exactly -9 to test
// it directly (attempts at the one other rung whose base is congruent to it
// mod 3 collapsed to the lava-sea floor instead of the target level, for a
// reason not yet understood — a future probe should chase that rather than
// assume -10 without it).
//
// WHAT IS STILL NOT MEASURED, and is marked rather than guessed:
//
//   * Whether a source already reading lava is exempt. Those cells sit below
//     the global lava sea, where nothing can be observed, so the conjunct is
//     carried on the spec's word alone.
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
/// considered. NARROWED, NOT YET PINNED to the single value: -11 and -10
/// both measure as lava, -8 as water, which brackets the true value to
/// {-10, -9} — -10 fits every reading measured so far, but -9 has not been
/// directly tested (`aquifer-fluidtype-probe.sh` group B/C — see this
/// file's own header for why not, and what the next attempt should try).
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
    return (at.level <= kLavaLevelCeiling && magnitude > kLavaThreshold) ? FluidType::Lava
                                                                         : FluidType::Default;
}

} // namespace stratum::aquifer
