// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Where two aquifer cells would place different things — one fluid, one air —
// the server writes a sheet of stone between them. SPEC recorded that as
// geometry with an unrecovered threshold "around 23", and that reading was
// wrong in an instructive way: 23 is not a constant of the algorithm, it is
// the value the real rule takes over the commonest geometry.
//
// The rule below is an exact, deterministic comparison. Every quantity in it
// is an integer, and there is no division anywhere, so no floorDiv arises —
// the only floating-point in it is the `barrier` router value, and only when
// the block sits within three of the nearer plane. It reproduces 251,658,240
// blocks across 40 probe dimensions on six world seeds, including a holdout
// generated after the rule was frozen.
#pragma once

#include <cstdint>

namespace stratum::aquifer {

/// The separation at which two sources stop competing. Beyond it no barrier is
/// written whatever the pressure says, and the clamp is real rather than
/// cosmetic: without it a negative similarity multiplied by a negative
/// pressure produces barriers, which was observed firing 478 times at a
/// `barrier` input of -1.0.
inline constexpr std::int32_t kSimilarityRange = 25;

/// The right-hand side of the comparison. Pinned by exact-equality cases: at
/// `barrier` 1.4999999, 1.5 and 1.5000001 the stone count over a probe world
/// is 191403, 191403 and 191405, so the two blocks that sit exactly on
/// `(25 - 20) * (6 + 6 * 1.5) = 75` flip at 1.5 and not before.
inline constexpr std::int32_t kBarrierPressure = 75;

/// How near the nearer plane a block must sit for the `barrier` router value
/// to enter at all. Beyond it the input has NO effect: thirteen barrier
/// constants from -1.0 to +4.0 give byte-identical output.
inline constexpr std::int32_t kBarrierReachAbove = 2; ///< on the air side, u <= 2
inline constexpr std::int32_t kBarrierReachBelow = 3; ///< on the fluid side, v <= 3

/// One block, weighed against the two aquifer sources nearest it.
struct BarrierAt {
    /// The block's y. The rule is per block, and the `barrier` value below is
    /// read at the block's own position rather than at either cell's centre.
    std::int32_t y = 0;

    /// `dB - dA`, the difference of the two SQUARED euclidean distances from
    /// the block's INTEGER position to the two nearest cell centres, with
    /// `dA <= dB`. Non-negative by construction.
    std::int32_t separation = 0;

    /// The two sources' fluid levels, from `cellFluidLevel`. A source's fluid
    /// occupies `y < level`.
    std::int32_t levelA = 0;
    std::int32_t levelB = 0;

    /// The `barrier` noise-router entry evaluated at this block.
    double barrier = 0.0;
};

/// REFUTED AS A GENERAL MODEL, and kept only until its replacement is built.
///
/// This predicate has no density term and sees only two sources. Measured
/// against the server across a sweep of the caller's density, its agreement
/// swings 92.88% at D = -0.05 — WORSE than the 99.99% majority baseline on
/// those same blocks — to 95.87% at D = -1.5 and 59.63% at D = -6.0. A rule
/// whose accuracy tracks an input it does not read is not a refinement of the
/// truth; it is a different model that happens to fit where that input is far
/// from zero.
///
/// What replaces it (spec Q6.6, shape confirmed, exact numerics not yet):
///
///     solid =  D + s12*P(A1,A2) > 0
///           || (s13 > 0 && D + s12*s13*P(A1,A3) > 0)
///           || (s23 > 0 && D + s12*s23*P(A2,A3) > 0)
///
/// with `D` the caller's density, `s_ij = 1 - (d_j - d_i)/25` on squared
/// distances, and three ranked sources rather than two — which is where the
/// 13% of the server's barriers this rule cannot see have been going. The
/// additive-D shape, its monotonicity, and the widening to three-way junctions
/// are confirmed (0 violations in 1053963 pairwise checks on two seeds); the
/// pressure function's own divisors are not yet measured, so the replacement
/// is not written here rather than written wrongly.
///
/// It cannot be wired yet in any case: nothing in this build selects three
/// ranked sources, and `BarrierAt` has no field for a third or for `D`.

/// Whether the server writes stone here.
///
/// The comparison is `(25 - separation) * pressure > 75`, and it is STRICT:
/// three independent cases that hit exact equality — u=4 with separation 20,
/// u=9 with separation 22, and v=2 at `barrier` 1.5 with separation 20 — all
/// place no barrier.
///
/// KNOWN INCOMPLETE, and this is why the filler still refuses aquifers: about
/// 13% of the server's real barriers come from a THIRD source rather than from
/// the nearest two, and this predicate cannot see them. It is exact on the
/// pairs it is given; it is not yet the whole barrier.
[[nodiscard]] bool placesBarrier(const BarrierAt& at) noexcept;

} // namespace stratum::aquifer
