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

/// THERE IS DELIBERATELY NO `preliminarySurfaceSample` HERE.
///
/// Its horizontal read is NOT a point sample, and shipping one would be worse
/// than shipping nothing. Three agents refuted the point-sample law
/// independently, and the sharpest of them needs no model at all: take two
/// worlds identical in seed, jitter, sea level, floodedness, noise field and
/// threshold, differing only in the LOW arm of the psl function. The partition
/// of cells into "samples the low arm" and "samples the high arm" is the same
/// under any position rule and under any aggregation over positions. 327 cells
/// that sample the HIGH arm in both are nonetheless entirely air in one world
/// and entirely water in the other, with byte-equal per-cell block counts.
/// No sample position can do that.
///
/// The best-supported reading is a MINIMUM-LIKE AGGREGATION over a horizontal
/// neighbourhood of order +-16 blocks at y = 0, which two agents reached
/// separately and which scores 1.000 / 0.934 / 1.000 / 1.000 / 1.000 at noise
/// wavelengths of 8, 32, 80, 160 and 400 blocks where the best point read
/// scores 0.44-0.93. But no support shape tried is exact everywhere, and the
/// aggregation does not cover the corner near the world floor, where a point
/// read at `floorDiv(centre.x, 4) * 4` and `floorDiv(centre.z, 4) * 4` is
/// exact on 21461 cells across six seeds and a minimum would flood every one
/// of them. There is an unexplained VALUE dependence there, and neither model
/// is shippable.
///
/// THE FAILURE MODE, recorded so the instrument is not rebuilt. All ~1370
/// earlier dimensions and the whole corpus that produced that exact 1.00000
/// held psl's low arm at -64. A readout that varies the spatial PATTERN of a
/// quantity and never its VALUES cannot see a value-dependent path, and
/// returns a confident, exactly-100%, wrong law. A future psl instrument must
/// sweep the arm values across the world floor, the lava level and the
/// ordinary surface range, not only the spatial frequency.
///
/// Until that is settled the filler refuses `aquifers_enabled` by name (SPEC
/// §8). psl is not a peripheral input: it decides the ocean gate, the
/// unconditional near-surface return, the depth term and the ladder's cap, and
/// a point read and a minimum over +-16 differ routinely by 5 to 20 blocks of
/// surface — enough to fill or empty a whole aquifer body.

} // namespace stratum::aquifer
