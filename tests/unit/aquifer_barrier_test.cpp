// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The cases below are the server's, not invented. The three equality cases in
// particular are why the comparison is spelled strict: each of them lands on
// `(25 - separation) * pressure == 75` exactly, and the server places no stone
// at any of them.
#include <stratum/aquifer/barrier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using stratum::aquifer::BarrierAt;
using stratum::aquifer::placesBarrier;

namespace {
// A block sitting between an air source below it and a fluid source above it.
// `above` is how far it clears the lower level, `below` how far it sits under
// the upper one.
BarrierAt between(const std::int32_t above, const std::int32_t below, const std::int32_t separation,
                  const double barrier = 0.0) {
    constexpr std::int32_t kY = 0;
    BarrierAt at;
    at.y = kY;
    at.levelA = kY - above; // the lower level: fluid occupies y < level, so
                            // this source puts air here
    at.levelB = kY + below; // the upper level: this one puts fluid here
    at.separation = separation;
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
    bothFluid.levelA = 10;
    bothFluid.levelB = 20;
    bothFluid.separation = 0;
    CHECK_FALSE(placesBarrier(bothFluid));
    // Both put air.
    BarrierAt bothAir = bothFluid;
    bothAir.levelA = -10;
    bothAir.levelB = -20;
    CHECK_FALSE(placesBarrier(bothAir));
}

TEST_CASE("the similarity is clamped at zero rather than going negative", "[aquifer]") {
    // Beyond the range there is no barrier however hard the pressure pushes.
    // Without the clamp a negative similarity times a negative `barrier` term
    // makes stone, which was observed firing 478 times at barrier -1.0.
    // This is the case that actually fires without the clamp: on the fluid side
    // at distance one the pressure is 4*1 - 2 + 6*(-1) = -4, and a separation
    // of 50 gives a similarity of -25, so the product is 100 — over the
    // threshold, from two negatives.
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
