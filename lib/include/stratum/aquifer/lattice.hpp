// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The aquifer is undocumented: minecraft.wiki says what its four router
// entries influence and gives no algorithm, and the only numeric constant
// published anywhere permitted is the 0.3 lava threshold. So everything here
// is a DERIVATION from the server's own output, measured through the open-void
// probe described in SPEC §11 — a dimension whose density is the constant -1,
// so no terrain exists and every boundary in the chunk belongs to the aquifer.
//
// WHAT IS SETTLED, and lives here:
//
//   * The cell PITCH: 16 x 12 x 16. Vertically from cell edges concentrating
//     14.8% at one residue mod 12 against 2.1% at another, with mod 6 flat;
//     horizontally from step rate by position within a candidate pitch, where
//     16 beats every other pitch by a factor of three and survives changing
//     the spread noise's frequency and splitting x from z.
//   * The FLUID LEVEL as a function of the spread, below. Ten values chosen
//     to separate two candidate forms refuted one of them on five of the ten.
//
// WHAT IS NOT SETTLED, and is deliberately absent rather than guessed:
//
//   * Where the grid is ANCHORED. What the probe sees is a cell boundary, and
//     a boundary's position is the grid origin convolved with however far the
//     cell's centre is jittered inside it. The pitch survives that convolution
//     and the origin does not, so there is no `cellOf` here yet: shipping one
//     would be picking an origin, and a wrong origin shifts every cell in the
//     world. SPEC §8 would rather have the gap than the guess.
//   * What sets the `base` the level is measured from. It tracks
//     `preliminary_surface_level` and does so non-linearly.
//   * The floodedness gate, the barrier rule, and the per-cell randomness that
//     moves 27% of columns with all four router inputs held constant.
#pragma once

#include <cstdint>

namespace stratum::aquifer {

/// The measured cell pitch. Horizontal from the step-rate test in both axes
/// at three spread frequencies, vertical from the residue histogram at three
/// world floors. Not the shape the wiki describes — the shape the server
/// produced.
inline constexpr std::int32_t kCellPitchX = 16;
inline constexpr std::int32_t kCellPitchY = 12;
inline constexpr std::int32_t kCellPitchZ = 16;

/// The vertical lattice is anchored in ABSOLUTE space: the identical probe at
/// `min_y` -64, -80 and -48 gives the same `y mod 12` histogram, shape
/// included, while `(y - min_y) mod 12` moves with the floor. So a dimension's
/// world floor does not shift the aquifer, which is the one anchoring question
/// that has been answered.
inline constexpr bool kVerticalLatticeIsAbsolute = true;

/// The fluid level a cell takes for a given `fluid_level_spread`, relative to
/// whatever base the cell is measured from.
///
/// Measured: sweeping the spread from -1 to 1 gives levels three apart with a
/// DOUBLED step at zero, which is a floor rather than a round, and the
/// transitions sit at spread = +-0.3, +-0.6, +-0.9. That is
///
///     3 * floorDiv(floor(spread * 10), 3)
///
/// and the bracketing puts the multiplier in (9.68, 10.34), consistent with
/// exactly ten. `3 * floor(spread * 3.5)` fits the coarse sweep equally well
/// and is wrong on five of the ten discriminating values.
///
/// Both roundings go toward negative infinity. A C++ `/` here would truncate
/// toward zero and disagree with vanilla for every cell below the base, which
/// is the failure §5's `floorDiv` rule exists to prevent — hence the helper
/// rather than the operator.
[[nodiscard]] std::int32_t spreadOffset(double spread) noexcept;

/// `base + spreadOffset(spread)`, which is the whole level once the base is
/// known. Spelled separately so that the part which IS derived can be tested
/// on its own while the base is still open.
[[nodiscard]] std::int32_t fluidLevel(std::int32_t base, double spread) noexcept;

} // namespace stratum::aquifer
