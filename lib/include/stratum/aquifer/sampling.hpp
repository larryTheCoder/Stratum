// Stratum — where the aquifer reads its router inputs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The level rule in `lattice.hpp` is settled to 99.99% per cell. Every probe
// behind it, though — about 1370 dimensions — held `preliminary_surface_level`
// and `fluid_level_floodedness` at CONSTANTS. So the predicate was settled and
// the positions its inputs are read AT were not, and in a real world the
// surface varies per column and feeds the depth directly. This header is the
// answer to that question, for the two inputs where there is one.
//
// Measured by six agents across eleven world seeds, on instruments built
// independently of each other. The headline is that the three inputs DO NOT
// share a sample position, and that is measured rather than inferred: on the
// same cells in the same worlds, the floodedness readout and the spread
// readout agree at 0.4895-0.5421 horizontally and 0.4986-0.5415 vertically,
// which is chance. Each was established on its own.
#pragma once

#include <stratum/aquifer/lattice.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace stratum::aquifer {

/// A position at which a noise-router entry is evaluated. Distinct from
/// `CellIndex` on purpose: two of the three reads below are NOT in block
/// coordinates, and conflating the two spaces is the mistake this type exists
/// to make hard.
struct SamplePos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    [[nodiscard]] constexpr bool operator==(const SamplePos&) const noexcept = default;
};

/// Where `fluid_level_floodedness` is read: the cell's own jittered centre, in
/// absolute block coordinates, verbatim. No quantisation, no offset, no
/// rounding, and no clamp — not even to the world floor, where a cell centred
/// below `min_y` still samples at its raw centre y.
///
/// One read per cell; the value is reused for every block that cell owns. A
/// per-block read is dead by three orders of magnitude rather than by a score:
/// it would have split 99.6% of cells under a one-block field and the server
/// split 4.8%.
///
/// The nearest rival is the centre quantised to two, at 0.618-0.627 against
/// this one's 1.0000, and it is wrong on all 136 cells where the two differ.
/// Also excluded on the same cells, all near or below the 0.498-0.574 chance
/// baseline: the cell's low corner, its midpoint, quantisation to 4, 8 and 16,
/// the centre plus or minus one on any axis (BELOW chance on y — a wrong
/// answer a majority-class baseline alone would have hidden), the cell index
/// as a coordinate, the jitter alone, every neighbouring cell, a fixed y at 0,
/// `min_y`, `sea_level` or the surface, the axes swapped, and the noise cell's
/// own corner. The worst of the full 16x16 grid of (x, z) candidates scores
/// 0.406; this one is first on every seed.
///
/// Spelled as a function of the centre rather than of the cell so that a
/// caller computes the centre once. It is the identity, and it is here to name
/// the finding and to make the asymmetry with `spreadSample` visible at the
/// call site.
[[nodiscard]] constexpr SamplePos floodednessSample(const CellIndex centre) noexcept {
    return SamplePos{.x = centre.x, .y = centre.y, .z = centre.z};
}

// `levelBand` — the 40-block band the spread is addressed by — lives in
// `lattice.hpp`, because the ladder is built from the same band.

/// Where `fluid_level_spread` is read — and it is NOT a position in block
/// space. The cell's lattice INDICES: the cell index in x and z, and the
/// 40-block band index in y.
///
/// Recovered without a candidate list, which is what makes it solid. Nine
/// dimensions each binary-encoding one bit of the sampled y gave per-band
/// purity of 1000/1000 on every bit and spelled the answer out directly; a
/// verifier repeated it with three independent 13-arm combs searching y over
/// [-2048, 2048] and read back the band index on all three of its own seeds.
/// The answer came off the server rather than out of a menu.
///
/// Excluded on the same cells: truncating the division (0.9417-0.9451), the
/// centre y itself — which is floodedness's own position — (0.4283-0.5416),
/// `floorDiv(centreY +- 20, 40)` (0.70-0.74), and every affine `40b + k` for k
/// in [-80, 120]. Horizontally: the centre (0.42-0.53), the corner
/// (0.43-0.53), the midpoint, any quantisation, and the axes swapped, all at
/// or below the 0.53-0.62 baseline. It is not an aggregate either: a minimum,
/// maximum or mean over the neighbouring bands scores 0.56-0.61, and a minimum
/// over the 3x3 index neighbourhood 0.43-0.47.
///
/// The x and z indices are `cell.x` and `cell.z` as given. `floorDiv(centre.x,
/// 16)` is provably the same number for every cell that can exist, since the
/// horizontal jitter never reaches 16, so the two spellings are a permanent
/// tie rather than an open question.
[[nodiscard]] SamplePos spreadSample(CellIndex cell, CellIndex centre) noexcept;

/// The one axis of `preliminary_surface_level`'s read that IS settled: it is
/// evaluated at absolute y = 0, whatever the cell's centre y, the cell layer,
/// the world's `min_y` or the dimension's `sea_level`.
///
/// Three independent confirmations, and one of them approaches from outside:
/// a psl that differs from a constant only on y in [-1, 1] changes every
/// block, one that differs only on y in [300, 310] changes nothing, and 0 of
/// 6291456 blocks differ from the constant world otherwise. So it is a read
/// near y = 0 and specifically NOT a minimum taken over y. A `y_clamped_gradient`
/// ladder brackets it to [-0.5, +0.5), which is exactly 0 for an integer.
/// Excluded: `min_y`, `min_y + 64`, `sea_level`, and the cell's own centre y.
inline constexpr std::int32_t kPreliminarySurfaceSampleY = 0;

/// How `preliminary_surface_level`'s read is anchored: the cell's own jittered
/// centre, quantised down to a multiple of four. `floorDiv`, never `/` — the
/// two agree everywhere the probe harness looked, because it hardcodes a
/// forceload of the origin quadrant, and that is exactly how a truncating
/// reading survived 1370 dimensions unnoticed. Off the origin they part
/// decisively: 1294 exact readings give floorDiv 1.0000 against truncation
/// 0.16-0.55.
inline constexpr std::int32_t kPslAnchorQuantum = 4;

/// The pitch of the window's lattice. Not the noise cell: `size_horizontal` 2
/// and 4 leave both this and the quantum unchanged (1.0000 on 356 readings,
/// while size-scaled variants score 0.09-0.55).
inline constexpr std::int32_t kPslStride = 16;

/// The value at which the scan gives up. ABSOLUTE, and strict: -61.99 and
/// -62.00 do not fire, -62.01 does, each at 1.0000. Invariant under `min_y` in
/// {-48, -64, -80, -96, -128} and `sea_level` in {40, 100, 128, 200}, which
/// kills the "world floor plus two" reading outright — at `min_y` -128 that
/// would put the constant at -126, so a -70 arm would not abort, and it does.
///
/// It coincides with `kLavaLevel - 8`. That identity is UNPROVEN: an arbitrary
/// constant fits everything measured just as well, and separating them needs a
/// world with `sea_level` below -54, which pushes every observable interface
/// into the one zone this campaign could not read.
inline constexpr double kPslAbortBelow = -62.0;

/// The window, as offsets from the anchor, IN SCAN ORDER. The anchor itself is
/// not in the list: it is read first and separately.
///
/// This is not a square, and the asymmetry is measured rather than assumed —
/// it reaches 48 blocks west and 16 east, north and south. Three independent
/// non-parametric sieves, on seven seeds and both coordinate signs, marked an
/// offset impossible the moment one cell contradicted it and each arrived at
/// exactly these thirteen positions out of thousands of candidates. Every
/// dense or symmetric shape loses: the 4x3 without the spur scores 0.9261, the
/// 5x3 0.8366, the 3x3 0.7082, the 4x4 0.6732, the 5x5 0.4436, and a point
/// read 0.0700. Adding the spur's mirror at `(+32, 0)`, or its neighbours at
/// `(-48, +-16)`, violates outright. Nobody can explain why it is asymmetric.
///
/// THE ORDER IS LOAD-BEARING, because the scan aborts. `dz` outer ascending,
/// `dx` inner ascending, with the spur first in its row. Pinned twice: 0 of
/// 200 random permutations reach the winning score and all eleven adjacent
/// transpositions lose; separately, 19 rival orders score at most 0.9756
/// against 1.0000, with x-outer at 0.82-0.89 and z-descending at 0.77-0.87.
inline constexpr std::size_t kPslWindowSize = 12;

/// One offset from the anchor. Horizontal only — the window has no vertical
/// extent, and cells sharing a column at different layers each get their own
/// anchor and are predicted exactly.
struct PslOffset {
    std::int32_t dx = 0;
    std::int32_t dz = 0;

    [[nodiscard]] constexpr bool operator==(const PslOffset&) const noexcept = default;
};

inline constexpr std::array<PslOffset, kPslWindowSize> kPslWindow{{
    {.dx = -32, .dz = -16},
    {.dx = -16, .dz = -16},
    {.dx = 0, .dz = -16},
    {.dx = 16, .dz = -16},
    {.dx = -48, .dz = 0},
    {.dx = -32, .dz = 0},
    {.dx = -16, .dz = 0},
    {.dx = 16, .dz = 0},
    {.dx = -32, .dz = 16},
    {.dx = -16, .dz = 16},
    {.dx = 0, .dz = 16},
    {.dx = 16, .dz = 16},
}};

// `PslRead` — the four values one scan yields — lives in `lattice.hpp`,
// because it is what the level rule consumes.

/// Read `preliminary_surface_level` for one cell.
///
/// This is the shape that defeated three campaigns, and none of the pieces is
/// guessable. It is not a point sample: two worlds differing only in the low
/// arm of the surface function put the same cells on the same side of any
/// conceivable sample position, and yet 327 cells that sample the high arm in
/// both are entirely air in one and entirely water in the other. It is a
/// minimum — rank 0, confirmed on exact integer readings rather than on bits,
/// with the second-smallest at 0.17-0.48 and the median, mean and maximum at
/// 0.0000. And it aborts on a value, which is what made the earlier campaigns
/// see a "point read near the world floor": on a TWO-valued field the aborting
/// prefix-minimum is identically the point read, so a corpus that never varied
/// psl's values could not tell them apart and reported an exact 1.00000 for a
/// law that is wrong. A three-valued field separates them at once — of 425
/// cells where the minimum is the low arm and the point read is the high arm,
/// 258 return the MIDDLE arm, which neither model predicts.
///
/// TWO CONSUMERS, ONE SCAN. The gate stops at the aborting sample and the cap
/// does not. The argument is arithmetic rather than a fit: with `sea_level` 40,
/// floodedness 0.6 and a ladder at -20, the settled level rule yields 40 or -20
/// for EVERY psl, and both leave the cell wet — yet 858 of 858 such cells with
/// a non-centre sample below -62 are observed dry at the lava level, which
/// needs `min(ladder, psl) <= -54` in a branch only reachable when
/// `psl >= sea_level - 8`. No single psl value satisfies both. Constant-psl
/// controls at -70, -64, -63, -62, -58, -54, -40, 0, 40 and 100 all flood
/// those same cells, so it is the spike and not the value that empties them.
///
/// @param psl    anything callable as `double(std::int32_t x, std::int32_t y,
///               std::int32_t z)` — the router entry, or a stub in a test.
/// @param centre the cell's jittered centre, from `CentreSource::centreOf`.
template<typename Sampler>
[[nodiscard]] PslRead readPreliminarySurface(const Sampler& psl, const CellIndex centre) {
    const std::int32_t anchorX =
        javamath::floorDiv(centre.x, kPslAnchorQuantum) * kPslAnchorQuantum;
    const std::int32_t anchorZ =
        javamath::floorDiv(centre.z, kPslAnchorQuantum) * kPslAnchorQuantum;

    // The anchor is read first and UNCONDITIONALLY, before any abort can fire.
    // Measured, not assumed: 200 of 200 cells whose own anchor sample is below
    // the threshold take that value rather than their neighbours' minimum.
    const double seed = psl(anchorX, kPreliminarySurfaceSampleY, anchorZ);
    double prefix = seed;
    double whole = seed;
    bool aborted = false;

    for (const auto& offset : kPslWindow) {
        const double value =
            psl(anchorX + offset.dx, kPreliminarySurfaceSampleY, anchorZ + offset.dz);
        if (!aborted) {
            if (value < kPslAbortBelow) {
                // The aborting sample is NOT folded into the gate's minimum,
                // and the scan stops there — it does not skip and continue.
                // "Skip and continue" scores 0.8662 and a row-only break
                // 0.8864, against 1.0000 for stopping outright.
                aborted = true;
            } else {
                prefix = std::min(prefix, value);
            }
        }
        whole = std::min(whole, value);
    }

    // Floor toward negative infinity, on the double, once. Not round (0.4855),
    // not truncation toward zero (0.9209 — failing exactly on the negatives).
    return PslRead{.gate = static_cast<std::int32_t>(std::floor(prefix)),
                   .cap = static_cast<std::int32_t>(std::floor(aborted ? whole : prefix)),
                   .anchor = static_cast<std::int32_t>(std::floor(seed)),
                   .aborted = aborted};
}

/// WHAT REMAINS A PERMANENT TIE, rather than an open measurement. `cap`'s value
/// on an aborting cell is only ever consumed through `max(-54, min(ladder, ·))`
/// and every aborting sample is below -62, so "the whole window's minimum",
/// "the aborting sample" and "any sentinel at or below -54" cannot be told
/// apart by any consumer that exists. The same saturation makes it undecidable
/// whether an abort also short-circuits the anchor read.

} // namespace stratum::aquifer
