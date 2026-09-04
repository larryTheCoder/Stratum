// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The vectors below are not invented: each is a level the vanilla server put
// in a chunk when the probe held `fluid_level_spread` at that constant, read
// off the water-to-air boundary in a world with no terrain in it. The base of
// -20 is the offset that configuration produced.
#include <stratum/aquifer/lattice.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using stratum::aquifer::fluidLevel;
using stratum::aquifer::spreadOffset;

namespace {
constexpr std::int32_t kMeasuredBase = -20;
}

TEST_CASE("the fluid level follows the spread the server was given", "[aquifer]") {
    // The coarse sweep: nine values a quarter apart. Steps of three, with the
    // doubled step between 0.00 and 0.25 that says this is a floor.
    CHECK(fluidLevel(kMeasuredBase, -1.00) == -32);
    CHECK(fluidLevel(kMeasuredBase, -0.75) == -29);
    CHECK(fluidLevel(kMeasuredBase, -0.50) == -26);
    CHECK(fluidLevel(kMeasuredBase, -0.25) == -23);
    CHECK(fluidLevel(kMeasuredBase, 0.00) == -20);
    CHECK(fluidLevel(kMeasuredBase, 0.25) == -20);
    CHECK(fluidLevel(kMeasuredBase, 0.50) == -17);
    CHECK(fluidLevel(kMeasuredBase, 0.75) == -14);
    CHECK(fluidLevel(kMeasuredBase, 1.00) == -11);
}

TEST_CASE("the spread's transitions sit where the server put them", "[aquifer]") {
    // These ten separate `3 * floorDiv(floor(s * 10), 3)` from
    // `3 * floor(s * 3.5)`, which fits the sweep above just as well and is
    // wrong on five of these. Each pair brackets a transition.
    CHECK(fluidLevel(kMeasuredBase, 0.29) == -20);
    CHECK(fluidLevel(kMeasuredBase, 0.31) == -17);
    CHECK(fluidLevel(kMeasuredBase, 0.58) == -17);
    CHECK(fluidLevel(kMeasuredBase, 0.62) == -14);
    CHECK(fluidLevel(kMeasuredBase, 0.85) == -14);
    CHECK(fluidLevel(kMeasuredBase, 0.88) == -14);
    CHECK(fluidLevel(kMeasuredBase, -0.29) == -23);
    CHECK(fluidLevel(kMeasuredBase, -0.31) == -26);
    CHECK(fluidLevel(kMeasuredBase, -0.58) == -26);
    CHECK(fluidLevel(kMeasuredBase, -0.62) == -29);
}

TEST_CASE("the grouping rounds toward negative infinity, not toward zero", "[aquifer]") {
    // The whole point of the floorDiv. A `/ 3` truncating toward zero agrees
    // on every non-negative value here and is one step high on every negative
    // one, which is why these are spelled out separately.
    CHECK(spreadOffset(0.0) == 0);
    CHECK(spreadOffset(0.29) == 0);
    CHECK(spreadOffset(0.31) == 3);
    CHECK(spreadOffset(1.0) == 9);

    CHECK(spreadOffset(-0.05) == -3); // floor(-0.5) = -1, floorDiv(-1, 3) = -1
    CHECK(spreadOffset(-0.1) == -3);  // floor(-1.0) = -1
    CHECK(spreadOffset(-0.29) == -3); // floor(-2.9) = -3, floorDiv(-3, 3) = -1
    CHECK(spreadOffset(-0.31) == -6); // floor(-3.1) = -4, floorDiv(-4, 3) = -2
    CHECK(spreadOffset(-1.0) == -12); // floor(-10) = -10, floorDiv(-10, 3) = -4

    // truncation would give -0 and -3 for these two rather than -3 and -6
    CHECK(spreadOffset(-0.05) != 0);
    CHECK(spreadOffset(-0.31) != -3);
}

TEST_CASE("the measured cell pitch is recorded as measured", "[aquifer]") {
    CHECK(stratum::aquifer::kCellPitchX == 16);
    CHECK(stratum::aquifer::kCellPitchY == 12);
    CHECK(stratum::aquifer::kCellPitchZ == 16);
    CHECK(stratum::aquifer::kVerticalLatticeIsAbsolute);
}

TEST_CASE("the lattice divides toward negative infinity on every axis", "[aquifer]") {
    using stratum::aquifer::CellIndex;
    using stratum::aquifer::cellOf;

    CHECK(cellOf(0, 0, 0) == CellIndex{0, 0, 0});
    CHECK(cellOf(15, 11, 15) == CellIndex{0, 0, 0});
    CHECK(cellOf(16, 12, 16) == CellIndex{1, 1, 1});

    // The cases a truncating division gets wrong: it would fold -1 and 0 into
    // one cell and shift every cell west, north and below the origin.
    CHECK(cellOf(-1, -1, -1) == CellIndex{-1, -1, -1});
    CHECK(cellOf(-16, -12, -16) == CellIndex{-1, -1, -1});
    CHECK(cellOf(-17, -13, -17) == CellIndex{-2, -2, -2});

    // The world floor of vanilla's overworld, which is not a multiple of 12.
    CHECK(cellOf(0, -64, 0).y == -6);
    CHECK(cellOf(0, -60, 0).y == -5);
}

TEST_CASE("the base sits on its own lattice, capped by the surface", "[aquifer]") {
    using stratum::aquifer::baseLevel;

    // The ladder read out of a spread-pinned world at psl 96: -20, 20, 60, 96.
    CHECK(baseLevel(-30, 96) == -20);
    CHECK(baseLevel(12, 96) == 20);
    CHECK(baseLevel(39, 96) == 20);
    CHECK(baseLevel(40, 96) == 60);
    CHECK(baseLevel(59, 96) == 60);

    // The cap is exact: the topmost level equalled psl on every value tried.
    CHECK(baseLevel(84, 56) == 56);
    CHECK(baseLevel(84, 96) == 96);
    CHECK(baseLevel(84, 160) == 100);
    CHECK(baseLevel(140, 160) == 140);
    CHECK(baseLevel(160, 160) == 160);

    // Below zero the lattice keeps its pitch only because the division floors.
    // Truncation would put y = -40 and y = -1 in one band and shift the ladder.
    CHECK(baseLevel(-40, 96) == -20);
    CHECK(baseLevel(-41, 96) == -60);
    // -1 is 19 from -20 and 21 from 20, so it takes the lower point.
    CHECK(baseLevel(-1, 96) == -20);
    CHECK(baseLevel(-64, 96) == -60);
}
