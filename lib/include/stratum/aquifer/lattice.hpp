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

#include <stratum/rng/xoroshiro128.hpp>

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
///
/// THE INDEX IS COMPUTED ON SHIFTED COORDINATES. This is the spec's Q3.2, and
/// it is confirmed: `(-5, +1, -5)` uniquely survived an elimination sweep over
/// all 270 shifts the geometry admits, on five seeds at 6291456 blocks each,
/// and was then re-derived from scratch by a second instrument on two further
/// seeds in both coordinate signs. 264 of the 266 wrong horizontal pairs are
/// refuted by at least one real barrier block they call impossible. The
/// vertical component was the last to fall: `+1` and `+2` differ on about
/// 0.0003% of blocks, and three targeted probe worlds built at those exact
/// coordinates came back barrier-stone where `+1` predicts possible and `+2`
/// predicts impossible, 3 of 3.
///
/// The shift is why a cell's own centre does NOT map back to that cell here.
/// `cellOf` names the HOME cell of the candidate window, not the owner of the
/// point: the window runs forward only in x and z, so the centre of cell `i`
/// lands in home cell `i - 1` or `i`, and cell `i` is a candidate either way.
inline constexpr std::int32_t kCellShiftX = -5;
inline constexpr std::int32_t kCellShiftY = 1;
inline constexpr std::int32_t kCellShiftZ = -5;

[[nodiscard]] CellIndex cellOf(std::int32_t x, std::int32_t y, std::int32_t z) noexcept;

/// The centre jitter, drawn once per 3D cell. It is an INTEGER draw, and the
/// bound differs by axis: ten values horizontally and nine vertically.
///
/// Recovered by search against the server, not from any source: a candidate
/// either reproduces a cell's draw or it does not, and this one does. On the
/// model-free readout — a cell at layer -4 takes the -20 fluid level rather
/// than the lava floor exactly when its centre clears y = -40, which for a
/// nine-valued draw means jy == 8 — it predicts **256 of 256 cells across four
/// world seeds**, with 34 predicted positive and the same 34 observed. Block
/// level, with no fitted table anywhere in the loop, it reproduces 78.4% of
/// the probe's barrier slabs exactly and places 99.3% of Voronoi boundaries
/// inside the observed slab; the shortfall is the barrier threshold, which is
/// still unrecovered, not the centres. Every ablation collapses to a 3-5% null
/// band: a different salt, dropping either fork, swapping the MD5 halves,
/// adding a third fork, or shifting by 15 or 17 instead of 16.
inline constexpr std::int32_t kJitterBoundX = 10;
inline constexpr std::int32_t kJitterBoundY = 9;
inline constexpr std::int32_t kJitterBoundZ = 10;

/// A cell's centre offset from the low corner of its cell.
struct Jitter {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    [[nodiscard]] constexpr bool operator==(const Jitter&) const noexcept = default;
};

/// The per-world source of cell centres. Immutable once built, and cheap to
/// query: the world seed is folded down to a 128-bit base once, and each cell
/// then costs one position mix and three draws.
class CentreSource {
public:
    /// Forks the world seed, salts it with the MD5 of `minecraft:aquifer`, and
    /// forks again — the same two-step derivation the noise registry uses,
    /// which is why `XoroshiroPositionalFactory` does the first half here.
    explicit CentreSource(std::int64_t worldSeed) noexcept;

    /// The three draws for one cell, in the order x, y, z from one generator.
    /// The order is not free: the other five assignments of draw slots to axes
    /// score 3.9-15.3% against this one's 78.9% at block level.
    [[nodiscard]] Jitter jitterOf(std::int32_t cx, std::int32_t cy, std::int32_t cz) const noexcept;

    /// The centre in absolute block coordinates.
    [[nodiscard]] CellIndex centreOf(std::int32_t cx, std::int32_t cy,
                                     std::int32_t cz) const noexcept;

    [[nodiscard]] constexpr rng::Seed128 base() const noexcept { return base_; }

private:
    rng::Seed128 base_;
};

/// The mix and the two-fork derivation live in `stratum::rng`: surface rules'
/// `vertical_gradient` turned out to use exactly the same primitive, so it is
/// not the aquifer's to own.

/// The pitch of the lattice the BASE fluid level sits on, which is not the
/// cell pitch and has no relation to it.
inline constexpr std::int32_t kBasePitch = 40;
inline constexpr std::int32_t kBasePhase = 20;

/// The 40-block band a cell's fluid level sits in — `floorDiv(centreY, 40)`.
///
/// The ladder is built from this band, and so is the address at which
/// `fluid_level_spread` is read (see `sampling.hpp`); the aquifer computes it
/// once and uses it for both, which is why it is spelled out rather than
/// inlined twice.
///
/// The jitter is INSIDE the division. `floorDiv(centreY, 40)` and
/// `floorDiv(12 * cellY, 40)` differ for five to nine cells per world, and the
/// second is wrong on every one of them — while scoring 0.9954-0.9975, which
/// is high enough to read as noise and is never 1.0000. It is also exactly
/// what an implementer writes by mistake.
[[nodiscard]] std::int32_t levelBand(std::int32_t centreY) noexcept;

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
/// vertical centre jitter (see `kJitterValues`) plus the aquifer's own stone
/// floor, about 2.4 blocks thick. Spending it needs the centre, which needs
/// the jitter DRAW — the one part of the geometry still unrecovered.
[[nodiscard]] std::int32_t baseLevel(std::int32_t y, std::int32_t preliminarySurface) noexcept;

/// The complete ladder level a cell takes when the floodedness gate sends it
/// there: the lattice point below the cell's own centre, moved by the spread,
/// capped by the preliminary surface, and floored at the lava sea.
///
/// The order is measured, not assumed. The cap is applied AFTER the spread
/// offset: at psl 67 a cell whose lattice point plus offset came to 69 was
/// observed at 67, not at 69 nor at 66.
///
/// The cap reads the scan's `cap`, not its `gate` — the two other candidates
/// score 0.9039 and 0.8719, and on the 6473 cells where they differ the server
/// backs `cap` on 3152 that `gate` gets wrong. And the lower clamp is
/// `lambdaLevel(seaLevel)` rather than a bare -54: at `sea_level` -100 with a
/// surface of -70, 79 cells read exactly -70 through a fluid band where -54
/// would have cut them off, and two campaigns found this from opposite sides.
[[nodiscard]] std::int32_t ladderLevel(std::int32_t centreY, std::int32_t cap, double spread,
                                       std::int32_t seaLevel) noexcept;

// ---------------------------------------------------------------------------
// The fluid-level decision, including the ocean branch.
// ---------------------------------------------------------------------------
//
// This corrects the previous entry outright. SPEC recorded the ocean branch as
// ONE bonus `max(0, 1 - d/K)` added to the floodedness before the same two
// gates, with K somewhere in 56.6-58.4 (five per-layer estimators) and later
// 55.32-55.56 (one per-cell fit). Both are wrong, and not by a little: there
// is no single bonus at all. The two gates carry DIFFERENT slopes, so no
// value of K can be right.
//
// The refutation does not depend on assuming the bonus is linear. Any one
// bonus added before two fixed thresholds forces the gap between the two
// crossing floodednesses to be constant in depth; measured, that gap runs
// 0.4750, 0.4500, 0.4125, 0.4000 at depths 8, 24, 48 and 56 or more. Two
// independent verifiers found this at different depths and on different seeds.
//
// The old per-cell bracket was an artefact of three things at once: it fitted
// one K to the sea gate only, over floodedness 0.60-0.79 only, at one
// preliminary surface only, through a readout that calls every cell centred
// below the lava level dry whatever its level. Both verifiers reproduced that
// failure mode from the description and named it.
//
// What replaced it was measured over roughly 1370 probe dimensions on 12 world
// seeds, preliminary surfaces from -54 to 141, sea levels 32 to 200, with the
// spread, the barrier and the lava level all varied. Four whole-model per-cell
// scores, never pooled across seeds: 99.9902%, 99.9806%, 99.9902%, 99.9858%.
// The same cells score 13-77% under the honest null (no ocean branch) and
// 86-97% under the K model this replaces.

/// How near the preliminary surface a cell's centre must be for the ocean
/// branch to flood it whatever the floodedness says. Bracketed to one block:
/// at a floodedness of -2.0, two full below any gate, depths 0 to 3 are still
/// the sea and depth 4 is dry.
inline constexpr std::int32_t kNearSurfaceDepth = 4;

/// How far below `sea_level` the preliminary surface must fall for the ocean
/// branch to run at all. Strict, and relative to the sea rather than absolute:
/// the boundary moves one-for-one with `sea_level` across 32, 40, 63, 100 and
/// 200, which excludes a hard-coded height.
inline constexpr std::int32_t kOceanGateOffset = 8;

/// The depth at which both of the ocean branch's bonuses reach zero. The two
/// fitted lines extrapolate to 55.900-56.000 and 55.951-56.000, and 56 is the
/// only integer in either; imposing it pins both slopes onto exact rationals.
inline constexpr std::int32_t kZeroBonusDepth = 56;

/// VINDICATED, and the scare is worth recording because this header carried
/// the opposite claim for a day. A campaign reported these scoring 0.21-0.69
/// on the cells that enter the reach path at floodedness 0.6, which read as
/// "fitted from wet/dry bits near the two thresholds, and wrong in the middle
/// of the band". It was not. Three mutually independent instruments on nine
/// seeds ran the same measurement with the surface held CONSTANT — which makes
/// all four psl consumers agree by construction — and got 1.00000 on every
/// reach-path cell.
///
/// The sharpest of them made floodedness a piecewise-constant function of the
/// sampled y, so ONE dimension carries a different floodedness at EVERY
/// integer depth. That brackets the bonus per depth rather than fitting a
/// slope, which kills "right at the ends, wrong in the middle" without
/// assuming linearity at all: |bonus(d) - (56-d)*11/640| < 1e-6 at every depth
/// 4..56, and exactly zero past the clamp out to 252.
///
/// What the failure actually was: the scan's ABORT (`PslRead::aborted`). The
/// control is decisive — a low arm of exactly -62, where nothing aborts,
/// scores 1.00000 under the model that has no abort term, and -63 collapses it
/// to 0.807.
///
/// Spelling either bonus as an amplitude over `kZeroBonusDepth` —
/// `1.05 * ((56.0 - d) / 56.0)` and its relatives — remains refuted: those
/// forms fire at depths where the server demonstrably does not, from two
/// independent sessions on different seeds.
/// The two slopes, and the whole reason the single-K reading is dead. 11/640
/// and 3/160 — different numbers. Their feasible intervals are disjoint by a
/// factor of 500 against their own widths and were tightened to 3.5e-8 by six
/// campaigns on thirteen seeds, with each constant strictly INTERIOR rather
/// than sitting at a bracket's closed edge.
///
/// SPELLED AS A NUMERATOR OVER A DENOMINATOR, and that is measured rather than
/// stylistic. A pre-divided `double` constant is REFUTED: `fl(11/640)` rounds
/// UP by 1.39e-18, and over the reachable range that is enough to fire the sea
/// gate where the server does not, at exactly two reaches — 45 and 50 — on 48
/// cells over thirteen dimensions, three seeds and both coordinate signs. The
/// surviving spellings are `f + (reach * 11.0) / 640.0`, its FMA, and the
/// cross-multiplied `640.0 * f + 11.0 * reach > 512.0`; they agree on every
/// one of 700000 grid points and on 145862 measured cells. `reach / 640.0 *
/// 11.0` and exact-rational comparison are both refuted on the server.
///
/// This also CORRECTS a claim this header used to make. `-ffp-contract=off`
/// remains project policy (§5), but it is not what makes this line right: with
/// the pre-divided constant the expression IS an FMA shape and the contracted
/// form happens to agree while the uncontracted one does not. Written as a
/// product over a divisor there is no multiply-add to contract, and the line
/// stops depending on contraction at all.
inline constexpr double kSeaBonusNumerator = 11.0;
inline constexpr double kSeaBonusDenominator = 640.0;
inline constexpr double kLocalBonusNumerator = 3.0;
inline constexpr double kLocalBonusDenominator = 160.0;

/// How far above the surface read an aborting near-surface cell must sit to
/// still take the sea. Exactly 20, pinned on eight purpose-built dimensions
/// where 19 and 21 each lose more than forty cells and are right on at most
/// one; the term reads the scan's `cap`, and it is independent of `sea_level`,
/// of the surface's high value and of the floodedness.
inline constexpr std::int32_t kNearSurfaceFloorOffset = 20;

/// The level a cell falls to when nothing floods it — and it is NOT always the
/// lava level. Every "-54" in this rule that is a LEVEL rather than a
/// threshold moves with `sea_level`.
///
/// Invisible until somebody mapped the global fluid picker with aquifers
/// disabled: below `min(-54, sea_level)` the world is lava unconditionally,
/// whatever the aquifer decides. So no block readout can distinguish any level
/// at or below that boundary — every candidate paints identical chunks — and
/// four campaigns read the lava sea's top and recorded it as a level. Building
/// a world with `sea_level` -56, where -54 sits two blocks ABOVE the floor,
/// makes the distinction visible: the third outcome is this, on 9740
/// discriminating cells, and a bare -54 is right on 8729 of them.
[[nodiscard]] constexpr std::int32_t lambdaLevel(const std::int32_t seaLevel) noexcept {
    return kLavaLevel < seaLevel ? kLavaLevel : seaLevel;
}

/// What one scan of `preliminary_surface_level` yields. Four values, because
/// the ocean branch has four consumers and they do not agree on which to read
/// (see `sampling.hpp` for how the scan produces them, and for why it is a
/// scan at all rather than a sample).
struct PslRead {
    /// The window's prefix minimum, stopping before any aborting sample. Read
    /// by the near-surface gate, the near-surface depth test, and the reach.
    std::int32_t gate = 0;

    /// The whole window's minimum, aborting sample included. Read by the
    /// ladder's cap and by the aborting near-surface floor.
    std::int32_t cap = 0;

    /// The anchor sample alone — the first read, before the window. Read by
    /// the DEPTH PATH's gate, and by nothing else.
    ///
    /// SINGLE-SOURCED. One agent, one instrument family, and an asymmetry
    /// (near-surface on the minimum, depth path on the anchor) of exactly the
    /// shape that has been wrong twice in this codebase. Its controls are good
    /// — worlds where the two models coincide score 1.0000 for both, and the
    /// effect moves one-for-one with `sea_level` — but it needs a second
    /// instrument before the filler leans on it.
    std::int32_t anchor = 0;

    /// Whether a window sample fell below the scan's threshold. When it did,
    /// the floodedness-gated `sea_level` outcome is REFUSED — which is the
    /// entire explanation of a failure this project first read as the slopes
    /// being wrong in the middle of the band.
    bool aborted = false;

    [[nodiscard]] constexpr bool operator==(const PslRead&) const noexcept = default;
};

/// The surface read of a dimension whose `preliminary_surface_level` is
/// constant in space. All four consumers then agree by construction, which is
/// why ~1370 probe dimensions could measure the slopes correctly and still
/// never see that there were four of them.
[[nodiscard]] constexpr PslRead constantSurface(const std::int32_t surface) noexcept {
    return PslRead{.gate = surface, .cap = surface, .anchor = surface, .aborted = false};
}

/// Everything the fluid-level decision reads. All of it is a function of the
/// cell's exact jittered CENTRE, never of the cell index: at psl 0 and
/// floodedness 0.6 the cell layer -4 spans y -48..-37 and splits inside
/// itself, centres -44..-40 taking water and -48..-45 taking air.
struct CellFluid {
    /// The cell's centre, from `CentreSource::centreOf`.
    std::int32_t centreY = 0;

    /// The scan of `preliminary_surface_level`, FLOORED to ints. Not
    /// truncated: at psl -10.4 and -10.6 the server behaves as -11 in both
    /// cases, while -10 behaves as -10; truncation toward zero, round-half and
    /// carrying the raw double each predict a different one of the four
    /// observed outcomes, and all three are excluded.
    ///
    /// Use `constantSurface(p)` for a dimension whose surface does not vary.
    PslRead surface{};

    /// The dimension's `sea_level`.
    std::int32_t seaLevel = 0;

    /// `fluid_level_floodedness`, sampled once per cell at the cell's own
    /// centre y. (The x and z of that sample, and the sample position of the
    /// surface and the spread, are NOT yet measured — see SPEC §11.)
    double floodedness = 0.0;

    /// `fluid_level_spread`.
    double spread = 0.0;
};

/// The level a cell's fluid body tops out at: fluid occupies `y < level`, so
/// `level` is the first air block above the body.
///
/// Both floodedness comparisons are STRICT, and that is measured rather than
/// assumed — at each of about forty integer depths the gate is dry at the
/// exact crossing floodedness and wet a ten-thousandth above it. Five of those
/// crossings are exactly representable in binary (0.25, 0.078125, -0.5,
/// -0.125, 0.4), so the strictness is settled with no floating-point
/// ambiguity at all.
///
/// FLOATING POINT, and it matters here. `floodedness + reach * slope` is
/// exactly the shape a fused multiply-add contracts, and with contraction on,
/// the outcome flips at sea-gate depths 6 and 11 precisely at the crossing.
/// §5's `-ffp-contract=off` / `/fp:precise` is load-bearing for this
/// expression, and those two depths are the natural x86-64/ARM64 canary.
///
/// The integer restatements `11*d < 640*f + 104` and `3*d < 160*f + 104` are
/// an oracle for reasoning, NOT the predicate: they agree with the form below
/// across the whole ten-thousandth grid except within about an ulp of a
/// crossing, where they differ because the crossing floodedness is not itself
/// representable.
[[nodiscard]] std::int32_t cellFluidLevel(const CellFluid& cell) noexcept;

} // namespace stratum::aquifer
