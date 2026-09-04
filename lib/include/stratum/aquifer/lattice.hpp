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
//   * Where the grid is ANCHORED. A boundary's position is the grid origin
//     convolved with the centre jitter, so boundaries could not separate the
//     two; a RUN's midpoint estimates the centre instead. Those midpoints put
//     the centre at `16k + j` with `E[j]` about 4.4 — in the LOWER HALF of its
//     own cell — which places the grid itself on multiples of sixteen.
//
// WHAT IS NOT SETTLED, and is deliberately absent rather than guessed:
//
//   * The jitter's distribution and the random source behind it. Only its
//     mean is measured, and `cellOf` below is the grid, not the centre.
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
/// included, while `(y - min_y) mod 12` moves with the floor.
///
/// The first version of that experiment was worthless and said so anyway. The
/// probe wrote `minecraft:overworld` as the dimension TYPE, and the type — not
/// the noise settings — fixes a world's height, so all three arms generated at
/// min_y -64 and two of them were the same world twice. The harness now ships
/// a dimension type of its own whenever the floor moves, and the re-run is a
/// real test: the worlds differ (lava reaches y = -78 at min_y -80 and stops
/// at -64 otherwise) while the aquifer geometry above the floor does not move.
inline constexpr bool kVerticalLatticeIsAbsolute = true;

/// The lava aquifer's level, and it is absolute too. At `min_y` -80 the lava
/// still tops out at y = -55 rather than at -71, which is what a floor-relative
/// level would give; at `min_y` -48 there is no lava at all, the world floor
/// being above it.
inline constexpr std::int32_t kLavaLevel = -54;

/// `fluid_level_floodedness` gates which level a cell takes, against two
/// constants that are exact to a ten-thousandth: at psl 96 a floodedness of
/// 0.4000 yields only the lava floor and 0.4001 yields the full ladder, while
/// 0.8000 is block-identical to 0.4001 and 0.8001 is the sea everywhere.
/// Both comparisons are strict, and the gate is DETERMINISTIC — every cell in
/// a world flips across that ten-thousandth, so no per-cell threshold wider
/// than 1e-4 can exist.
inline constexpr double kFloodedLocalThreshold = 0.4;
inline constexpr double kFloodedSeaThreshold = 0.8;

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

/// Which lattice cell a block belongs to.
struct CellIndex {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    [[nodiscard]] constexpr bool operator==(const CellIndex&) const noexcept = default;
};

/// The grid, anchored on multiples of the pitch in all three axes.
///
/// Horizontally that anchor is measured: run midpoints put the cell centre at
/// `16k + 4.4`, inside the lower half of the cell `[16k, 16k + 16)`, and the
/// figure holds across three world seeds and three spread frequencies.
/// Vertically the same holds, though the first reading of it was wrong. The
/// figure of `y = 8 (mod 12)` came from the highest AIR block, and an aquifer
/// rests on a stone floor about 2.4 blocks thick, so that estimator sits
/// several blocks below the interface it was taken for. The two clean
/// estimators — the floor's own bottom and the fluid's bottom — bracket the
/// interface at 9.68 and 11.61 (mod 12) over 262119 interfaces on four seeds,
/// which puts the vertical centre near `12k + 4.6` rather than `12k + 2`.
/// That is the same offset the horizontal axes give, so one jitter law covers
/// all three; the cell is `[12k, 12k + 12)` either way, which is why `cellOf`
/// is unaffected.
///
/// Every axis floors toward negative infinity: below y = 0, and west or north
/// of the origin, a truncating division would fold two cells into one.
[[nodiscard]] CellIndex cellOf(std::int32_t x, std::int32_t y, std::int32_t z) noexcept;

/// The pitch of the lattice the BASE fluid level sits on, which is not the
/// cell pitch and has no relation to it.
inline constexpr std::int32_t kBasePitch = 40;
inline constexpr std::int32_t kBasePhase = 20;

/// The base a cell's fluid level is measured from, before the spread moves it.
///
/// Measured by pinning the spread to zero — which makes the offset exactly
/// zero, so the level read out of a chunk IS the base — and reading every cell
/// rather than one number per world. The levels then land on a lattice of
/// pitch 40 anchored at `y = 20 (mod 40)`: -20, 20, 60, 100, 140. The
/// preliminary surface caps it, and does so exactly: across psl 56, 80, 96,
/// 128 and 160 the topmost level was 56, 80, 96, 128 and 160.
///
/// The lattice is the aquifer's own. It does not move with `sea_level` (32 and
/// 96 give an identical ladder), and raising the sea through it merges bodies
/// rather than shifting them.
///
/// APPROXIMATE IN ONE RESPECT, deliberately. A cell takes the lattice point
/// nearest its own CENTRE, and centres are jittered, so the transition between
/// two lattice points is smeared rather than sharp. Taking @p y as the centre
/// predicts 96.1% of blocks over 6.1 million, and every disagreement is within
/// about ten blocks of a lattice boundary — 78% wrong at y = -40, 0, 40 and 80
/// and under 2% by ten blocks away.
///
/// That residual is now ACCOUNTED FOR rather than merely bounded: it is the
/// vertical centre jitter, uniform on about `[0, 9.2)`, plus the aquifer's own
/// 2.4-block stone floor. Spending it needs the centre, which needs the jitter
/// draw, which is the one part of the geometry still unmeasured.
[[nodiscard]] std::int32_t baseLevel(std::int32_t y, std::int32_t preliminarySurface) noexcept;

} // namespace stratum::aquifer
