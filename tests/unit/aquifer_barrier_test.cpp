// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The two-source cases below are the server's, not invented — they are the
// same 251,658,240-block validation the old two-source-only rule carried,
// now expressed through the three-source `BarrierAt`, with the third source
// pushed far enough away (`distanceSq` a million) that `s13` and `s23` are
// always <= 0 and it never competes; `density` is fixed at -1.0, the value
// the old rule was implicitly built for (barrier.hpp's own header derives
// the equivalence). The three equality cases in the first test are why the
// comparison is spelled strict: each lands on the boundary exactly, and the
// server places no stone at any of them.
//
// The genuine three-source case at the bottom is new: barrier.hpp's own
// header describes the real-server probe that found it, but the numbers
// here are hand-built and independently verified by direct calculation
// (not read off a probe), to pin the SHAPE of a rescue rather than one
// specific measured block.
#include <stratum/aquifer/barrier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using stratum::aquifer::BarrierAt;
using stratum::aquifer::BarrierSource;
using stratum::aquifer::placesBarrier;

namespace {
constexpr std::int64_t kInertDistanceSq = 1'000'000; // s13, s23 <= 0 regardless of the pair.

// A block sitting between an air source below it and a fluid source above it.
// `above` is how far it clears the lower level, `below` how far it sits under
// the upper one. The lower level is the NEARER source (`distanceSq` 0); the
// upper is `separation` further out — matching `dA <= dB` from the old
// two-source rule this reproduces.
BarrierAt between(const std::int32_t above, const std::int32_t below, const std::int32_t separation,
                  const double barrier = 0.0) {
    constexpr std::int32_t kY = 0;
    BarrierAt at;
    at.y = kY;
    at.density = -1.0;
    at.nearest = BarrierSource{.level = kY - above, .distanceSq = 0};
    at.second = BarrierSource{.level = kY + below, .distanceSq = separation};
    at.third = BarrierSource{.level = kY - above, .distanceSq = kInertDistanceSq};
    at.barrier = barrier;
    return at;
}
} // namespace

TEST_CASE("the comparison is strict where the server's own cases land on it", "[aquifer]") {
    // (25 - 20) * (2*4 + 7) = 5 * 15 = 75.
    CHECK_FALSE(placesBarrier(between(4, 40, 20)));
    // (25 - 22) * (2*9 + 7) = 3 * 25 = 75.
    CHECK_FALSE(placesBarrier(between(9, 40, 22)));
    // (25 - 20) * (4*2 - 2 + 6*1.5) = 5 * 15 = 75, on the fluid side, and this
    // is the one the stone counts pinned: 191403 / 191403 / 191405 blocks at
    // barrier 1.4999999 / 1.5 / 1.5000001.
    CHECK_FALSE(placesBarrier(between(40, 2, 20, 1.4999999)));
    CHECK_FALSE(placesBarrier(between(40, 2, 20, 1.5)));
    CHECK(placesBarrier(between(40, 2, 20, 1.5000001)));
}

TEST_CASE("a pair that agrees writes nothing", "[aquifer]") {
    // Both sources put fluid here.
    BarrierAt bothFluid;
    bothFluid.y = 0;
    bothFluid.density = -1.0;
    bothFluid.nearest = BarrierSource{.level = 10, .distanceSq = 0};
    bothFluid.second = BarrierSource{.level = 20, .distanceSq = 0};
    bothFluid.third = BarrierSource{.level = 10, .distanceSq = kInertDistanceSq};
    CHECK_FALSE(placesBarrier(bothFluid));
    // Both put air.
    BarrierAt bothAir = bothFluid;
    bothAir.nearest.level = -10;
    bothAir.second.level = -20;
    bothAir.third.level = -10;
    CHECK_FALSE(placesBarrier(bothAir));
}

TEST_CASE("the similarity is clamped at zero rather than going negative", "[aquifer]") {
    // Beyond the range there is no barrier however hard the pressure pushes.
    // Without the clamp a negative similarity times a negative `barrier` term
    // makes stone, which was observed firing 478 times at a `barrier` input
    // of -1.0. This is the case that actually fires without the clamp: on
    // the fluid side at distance one the pressure is 4*1 - 2 + 6*(-1) = -4,
    // and a separation of 50 gives a similarity of -25, so the product is
    // 100 — over the threshold, from two negatives.
    CHECK_FALSE(placesBarrier(between(40, 1, 50, -1.0)));
    CHECK_FALSE(placesBarrier(between(0, 1, 25, 4.0)));
    // Just inside the range, the same block does.
    CHECK(placesBarrier(between(0, 1, 20, 4.0)));
}

TEST_CASE("the router value reaches only three blocks from the nearer plane", "[aquifer]") {
    // Thirteen barrier constants from -1.0 to +4.0 give byte-identical output
    // beyond that reach, so these must not move.
    for (const double barrier : {-1.0, 0.0, 1.0, 4.0}) {
        INFO("barrier " << barrier);
        CHECK(placesBarrier(between(3, 40, 20, barrier)) == placesBarrier(between(3, 40, 20, 0.0)));
        CHECK(placesBarrier(between(40, 4, 20, barrier)) == placesBarrier(between(40, 4, 20, 0.0)));
    }
    // Inside the reach it does move things: on the air side at distance two,
    // (25 - 14) * (2*2 + 7 + 6b) crosses 75 between b = 0 and b = -1.
    CHECK(placesBarrier(between(2, 40, 14, 0.0)));
    CHECK_FALSE(placesBarrier(between(2, 40, 14, -1.0)));
}

TEST_CASE("a tie between the two planes goes to the lower one", "[aquifer]") {
    // The block centre is `above + 0.5` from the lower plane and
    // `below - 0.5` from the upper, so a tie needs below == above + 1, which
    // only happens for odd gaps. The two sides carry different pressures —
    // 2u + 7 against 4v - 2 — so the choice is observable: at above = 6 the
    // lower side gives 19 and the upper would give 26, and a separation of 21
    // sits between them (4 * 19 = 76 > 75, and both exceed it, so pick a
    // separation where they part).
    const std::int32_t above = 6;
    const std::int32_t below = above + 1;
    // (25 - 22) * 19 = 57, no stone; the upper side would give
    // (25 - 22) * 26 = 78, which would.
    CHECK_FALSE(placesBarrier(between(above, below, 22)));
}

TEST_CASE("Q6.2's distance gate short-circuits before any third-source term fires", "[aquifer]") {
    // s12 <= 0 (separation >= 25) refuses the FIRST term outright, per the
    // old two-source rule — and per Q6.2, it refuses the whole predicate:
    // "no barrier evaluation and no noise consumed", not merely the first
    // term. A third source close enough and disagreeing hard enough to fire
    // on its own must not rescue it.
    BarrierAt at;
    at.y = 10;
    at.density = -1.0;
    at.nearest = BarrierSource{.level = 0, .distanceSq = 0};   // air at y=10
    at.second = BarrierSource{.level = 100, .distanceSq = 25}; // fluid at y=10; s12 == 0
    at.third = BarrierSource{.level = 100, .distanceSq = 1};   // fluid at y=10, very near
    CHECK_FALSE(placesBarrier(at));
}

TEST_CASE("a third source rescues a block the nearest two alone would miss", "[aquifer]") {
    // Independently verified by direct calculation of Q6.4/Q6.6, not read
    // off a probe: A1 (air, level 0) and A2 (fluid, level 100) disagree at
    // y=10 but sit far enough apart (s12=0.8) that D=-8 clears their own
    // term — pressure(0,100,10,0) = 9.0, and -8 + 0.8*9.0 = -0.8 <= 0. A3
    // (air, level -900) sits close to A2 (s23=0.96) and disagrees with it
    // hard: the gap is so wide (1000) that pressure(100,-900,10,0) reaches
    // 119.33, and -8 + 0.8*0.96*119.33 = 83.6 > 0. A1 and A3 themselves
    // AGREE at y=10 (both air), so the A1-A3 term never fires — the rescue
    // is entirely the A2-A3 term's.
    BarrierAt at;
    at.y = 10;
    at.density = -8.0;
    at.nearest = BarrierSource{.level = 0, .distanceSq = 0};
    at.second = BarrierSource{.level = 100, .distanceSq = 5};
    at.third = BarrierSource{.level = -900, .distanceSq = 6};

    // The nearest two alone: no barrier.
    BarrierAt twoSourceOnly = at;
    twoSourceOnly.third = BarrierSource{.level = 0, .distanceSq = kInertDistanceSq};
    CHECK_FALSE(placesBarrier(twoSourceOnly));

    // With the third source, the A2-A3 disagreement rescues it.
    CHECK(placesBarrier(at));
}
