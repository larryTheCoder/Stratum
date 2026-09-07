// Stratum — which aquifer sources compete for a block.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `lattice.hpp` says what fluid ONE cell holds. `barrier.hpp` says what
// happens where TWO of them disagree. Nothing said which cells those are, and
// that gap is why the filler still refuses aquifers: `placesBarrier` has only
// ever been called with two levels handed to it by a caller that does not
// exist in this tree, and about 13% of the server's real barriers come from a
// THIRD source that no two-source caller could ever supply.
//
// This file is that missing layer. It is the first code in `lib/` written
// against the clean-room specification of SPEC §12 rather than derived here
// first, so every claim below carries BOTH halves §12 requires — the spec's
// claim and the measurement that confirmed it — or is marked untested.
//
// WHAT IS CONFIRMED AGAINST THE SERVER:
//
//   * The cell index is computed on shifted coordinates (spec Q3.2). That
//     lives in `lattice.hpp`; it is the unique survivor of an elimination
//     sweep over all 270 shifts the geometry admits.
//   * The metric is the INTEGER squared euclidean distance from the block's
//     own position — not from its centre (spec Q4.2). 1742/1742 and 1690/1690
//     against 0/n for a `+0.5` variant.
//   * Four ranks are kept, and the fourth never reaches the substance
//     decision (spec Q4.3). 2582 blocks chosen because two models disagree on
//     rank 4 alone.
//   * Ties displace toward the LATER candidate, at every rank (spec Q4.4).
//     228/228 and 334/334, where this project's own earlier "first wins"
//     scores 12.3%.
//
// WHAT IS NOT, and is marked rather than passed through:
//
//   * The WINDOW itself (spec Q4.1). The asymmetric set below and the
//     symmetric 27-cell set agree on the NEAREST source on all but about two
//     blocks in a million, and where they part at rank 2 the nearest pair has
//     already stopped competing — so Q6.2's short-circuit hides the difference
//     in every block the server writes. That is why 6291456-block barrier
//     corpora scored the two identically, and it is reproduced here from this
//     build's own centres. It is adopted as the spec's hypothesis, and
//     `kWindowIsUntested` says so at the call site.
//
// WHAT THE WHOLE LAYER SCORES AGAINST THE SERVER, which is the part no unit
// vector can supply. Two readouts of the open-void probe, neither of them
// touching the refuted barrier predicate (tests/conformance):
//
//   * 637252 barrier blocks the server wrote, on four seeds. Every one has
//     `d2 - d1 < 25`, which is the only separation at which any of Q6.6's
//     three terms can fire. The bound is TIGHT — about two thousand blocks sit
//     at exactly 24 — and the same rule with the shift of Q3.2 removed puts
//     3.16% of the server's own stone where it says no barrier can exist.
//   * 16663703 non-solid blocks, where the barrier has fallen through and the
//     substance is the nearest source's own reading. `y < cellFluidLevel(rank
//     1)` predicts 0.99993 to 0.99996 of them per seed. The residual is not
//     the selection: a brute-force search over a 5x7x5 neighbourhood of cells
//     fixes 0 of it.
#pragma once

#include <stratum/aquifer/lattice.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace stratum::aquifer {

/// One cell offset from the home cell.
struct CandidateOffset {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    std::int32_t dz = 0;

    [[nodiscard]] constexpr bool operator==(const CandidateOffset&) const noexcept = default;
};

inline constexpr std::size_t kCandidateCount = 12;

/// UNTESTED, deliberately flagged. The window is the one part of the selection
/// layer no instrument in this project has separated from its rivals: the
/// asymmetric set and the symmetric 27-cell set produce the same first two
/// ranks everywhere a barrier can form, so every barrier measurement scores
/// them identically. It is here as the spec's claim, not as a finding.
inline constexpr bool kWindowIsUntested = true;

/// The candidate cells, as offsets from the home cell, IN ITERATION ORDER.
///
/// Forward only in x and z, symmetric in y — which is what makes the shift of
/// Q3.2 necessary rather than cosmetic: without it a forward-only window would
/// sit entirely to one side of its own block. Measured, over 786432 blocks on
/// five seeds: with the shift the block lies between the lowest and highest
/// candidate centre on 0.9978 to 1.0000 of blocks horizontally and on exactly
/// all of them vertically; without it, 0.855 to 0.888. So the spec's "this
/// still brackets the block" is very nearly right rather than exactly right,
/// and the shortfall is a property of the geometry, not of this build.
///
/// THE ORDER IS LOAD-BEARING because ties displace (see `rankCandidates`). It
/// is x outermost, y in the middle, z innermost. A different order changes
/// which of two equidistant cells occupies a rank, and ties are common rather
/// than measure-zero — the spec reports duplicate squared distances in 15.40%
/// of selections, with rank 1-2 alone at 1.079%.
inline constexpr std::array<CandidateOffset, kCandidateCount> kCandidateWindow{{
    {.dx = 0, .dy = -1, .dz = 0},
    {.dx = 0, .dy = -1, .dz = 1},
    {.dx = 0, .dy = 0, .dz = 0},
    {.dx = 0, .dy = 0, .dz = 1},
    {.dx = 0, .dy = 1, .dz = 0},
    {.dx = 0, .dy = 1, .dz = 1},
    {.dx = 1, .dy = -1, .dz = 0},
    {.dx = 1, .dy = -1, .dz = 1},
    {.dx = 1, .dy = 0, .dz = 0},
    {.dx = 1, .dy = 0, .dz = 1},
    {.dx = 1, .dy = 1, .dz = 0},
    {.dx = 1, .dy = 1, .dz = 1},
}};

/// One cell in the window, with its centre already drawn.
struct Candidate {
    CellIndex cell{};
    CellIndex centre{};

    [[nodiscard]] constexpr bool operator==(const Candidate&) const noexcept = default;
};

/// The twelve cells that compete for a block, in iteration order.
///
/// Takes the HOME cell rather than the block so that a caller which walks a
/// column computes `cellOf` once per cell rather than once per block. The home
/// cell is `cellOf(x, y, z)` — already shifted.
[[nodiscard]] std::array<Candidate, kCandidateCount> candidatesFor(const CentreSource& centres,
                                                                   CellIndex home) noexcept;

/// The squared euclidean distance from a block's own INTEGER position to a
/// candidate centre.
///
/// Integer throughout, and that is measured rather than a convenience: the
/// distance is taken from `(x, y, z)` and NOT from the block centre
/// `(x + 0.5, ...)`. The half-block variant scores 0 of 1742 and 0 of 1690 on
/// blocks chosen because the two disagree.
///
/// It cannot overflow. Within the window `|dx|` and `|dz|` are at most 20 and
/// `|dy|` at most 22, so the result never exceeds 1284 — which is why this
/// returns `int32` rather than the `int64` a general distance would need.
[[nodiscard]] constexpr std::int32_t squaredDistanceTo(const CellIndex centre, const std::int32_t x,
                                                       const std::int32_t y,
                                                       const std::int32_t z) noexcept {
    const std::int32_t dx = x - centre.x;
    const std::int32_t dy = y - centre.y;
    const std::int32_t dz = z - centre.z;
    return (dx * dx) + (dy * dy) + (dz * dz);
}

inline constexpr std::size_t kRankCount = 4;

/// The distance of a rank no candidate reached. Unreachable from the real
/// window, which always fills all four; it exists so that `rankCandidates` has
/// a defined answer for the short spans a test hands it.
inline constexpr std::int32_t kNoSource = std::numeric_limits<std::int32_t>::max();

/// One ranked source: a cell, its centre, and how far the block is from it.
struct Source {
    CellIndex cell{};
    CellIndex centre{};
    std::int32_t distanceSq = kNoSource;

    [[nodiscard]] constexpr bool operator==(const Source&) const noexcept = default;
};

/// The four nearest sources, nearest first.
///
/// Rank 4 is retained and must NOT be fed to the substance decision: it
/// reaches only the fluid-update flag (spec Q4.3, confirmed on 2582 blocks
/// chosen because two models disagree on rank 4 alone). This build has no
/// fluid-update flag yet, so rank 4 is currently carried and unused — kept
/// because dropping it would change which cell occupies rank 3 the moment a
/// rank 3-4 tie arises, and those are the ties that move the most blocks.
struct Selection {
    std::array<Source, kRankCount> ranked{};

    [[nodiscard]] constexpr const Source& nearest() const noexcept { return ranked[0]; }

    /// `d2 - d1`, which is what the barrier's similarity is built from. A
    /// barrier can exist only where this is under `kSimilarityRange`: all
    /// three of the spec's Q6.6 terms carry `s12` as a factor, so `s12 <= 0`
    /// short-circuits to the nearest source outright (spec Q6.2, confirmed).
    [[nodiscard]] constexpr std::int32_t separation() const noexcept {
        return ranked[1].distanceSq - ranked[0].distanceSq;
    }
};

/// Rank a set of candidates against a block position.
///
/// TIES DISPLACE TOWARD THE LATER CANDIDATE. A candidate whose squared
/// distance EQUALS the value standing at a rank takes that rank and cascades
/// the previous occupant down, and this holds independently at all four —
/// spec Q4.4, confirmed here at 228/228 and 334/334 against the server, where
/// this project's own "first wins" reading scores 12.3%. Which is to say the
/// comparisons below are `<=` and every one of them matters.
///
/// Spelled to take a span rather than the window so that the ranking rule can
/// be given known-answer vectors without a world seed anywhere in the loop.
/// The real caller always passes twelve.
[[nodiscard]] Selection rankCandidates(std::int32_t x, std::int32_t y, std::int32_t z,
                                       std::span<const Candidate> candidates) noexcept;

/// The whole selection for one block: home cell, window, metric, ranking.
///
/// THE WINDOW DOES NOT ALWAYS CONTAIN THE TRUE NEAREST CENTRE, and this
/// corrects a claim this project made before it looked hard enough. Because
/// the window runs forward only in x and z, a cell outside it is occasionally
/// nearer than every cell inside. Re-measured here against a brute-force
/// search over a 7x9x7 neighbourhood, 3538944 blocks on each of four seeds:
/// 7 to 20 rank-1 misses (2.0e-6 to 5.7e-6) and 7863 to 9474 at rank 2
/// (2.2e-3 to 2.7e-3). An earlier note read "0 exceptions in 1105920 blocks",
/// which was a corpus too small to see the rate rather than a proof.
///
/// That is not a defect to fix. The server's own window is forward-only, so
/// its selection misses the same way; a "corrected" symmetric window would
/// disagree with vanilla exactly where this one looks wrong.
[[nodiscard]] Selection selectSources(const CentreSource& centres, std::int32_t x, std::int32_t y,
                                      std::int32_t z) noexcept;

} // namespace stratum::aquifer
