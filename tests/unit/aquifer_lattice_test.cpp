// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The vectors below are not invented: each is a level the vanilla server put
// in a chunk when the probe held `fluid_level_spread` at that constant, read
// off the water-to-air boundary in a world with no terrain in it. The base of
// -20 is the offset that configuration produced.
#include <stratum/aquifer/lattice.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
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

TEST_CASE("the measured aquifer constants are recorded as measured", "[aquifer]") {
    // Bracketed to a ten-thousandth against the server: at psl 96, floodedness
    // 0.4000 gives the lava floor alone and 0.4001 the whole ladder; 0.8000 is
    // block-identical to 0.4001 and 0.8001 is sea everywhere.
    // Spelled through the bit pattern: the project set builds with
    // -Werror=float-equal, and these are exact constants rather than results
    // of arithmetic, so bit equality is the assertion actually wanted.
    CHECK(std::bit_cast<std::uint64_t>(stratum::aquifer::kFloodedLocalThreshold) ==
          std::bit_cast<std::uint64_t>(0.4));
    CHECK(std::bit_cast<std::uint64_t>(stratum::aquifer::kFloodedSeaThreshold) ==
          std::bit_cast<std::uint64_t>(0.8));

    // Absolute, not measured from the world floor: at min_y -80 the lava still
    // tops out at -55 rather than the -71 a floor-relative level would give.
    CHECK(stratum::aquifer::kLavaLevel == -54);
    CHECK(stratum::aquifer::kVerticalLatticeIsAbsolute);

    // The base lattice is clamped below by the lava level wherever it would
    // otherwise fall through it.
    CHECK(stratum::aquifer::baseLevel(-64, 96) < stratum::aquifer::kLavaLevel + 12);
}

TEST_CASE("the centre jitter draws ten, nine and ten", "[aquifer]") {
    using stratum::aquifer::CentreSource;
    using stratum::aquifer::Jitter;

    // Known answers from the derivation recovered against the server. They are
    // self-consistent by construction; what ties them to vanilla is the
    // conformance case, which scores this same code on the server's own blocks.
    const CentreSource s42{42};
    CHECK(s42.jitterOf(0, 0, 0) == Jitter{7, 7, 9});
    CHECK(s42.jitterOf(0, -4, 0) == Jitter{9, 4, 6});
    CHECK(s42.jitterOf(3, -4, 5) == Jitter{6, 7, 8});
    CHECK(s42.jitterOf(-1, 1, -1) == Jitter{0, 6, 4});
    CHECK(s42.jitterOf(7, 7, 7) == Jitter{2, 1, 8});

    const CentreSource s7{7};
    CHECK(s7.jitterOf(0, 0, 0) == Jitter{9, 6, 8});
    CHECK(s7.jitterOf(3, -4, 5) == Jitter{0, 6, 2});

    // A negative seed, since the position mix sign-extends and shifts
    // arithmetically, and every step of it wraps.
    const CentreSource sneg{-1};
    CHECK(sneg.jitterOf(0, 0, 0) == Jitter{2, 7, 4});
    CHECK(sneg.jitterOf(7, 7, 7) == Jitter{8, 8, 5});
}

TEST_CASE("the jitter fills its per-axis bounds and no more", "[aquifer]") {
    // Ten horizontally, nine vertically — not one width on every axis, which is
    // what the measurement first suggested. Over 32000 cells every value inside
    // each bound appears and nothing outside it does.
    const stratum::aquifer::CentreSource source{42};
    std::array<int, 16> seenX{};
    std::array<int, 16> seenY{};
    std::array<int, 16> seenZ{};
    for (std::int32_t cx = -20; cx < 20; ++cx)
        for (std::int32_t cy = -6; cy < 14; ++cy)
            for (std::int32_t cz = -20; cz < 20; ++cz) {
                const auto jitter = source.jitterOf(cx, cy, cz);
                REQUIRE(jitter.x >= 0);
                REQUIRE(jitter.y >= 0);
                REQUIRE(jitter.z >= 0);
                REQUIRE(jitter.x < stratum::aquifer::kJitterBoundX);
                REQUIRE(jitter.y < stratum::aquifer::kJitterBoundY);
                REQUIRE(jitter.z < stratum::aquifer::kJitterBoundZ);
                ++seenX.at(static_cast<std::size_t>(jitter.x));
                ++seenY.at(static_cast<std::size_t>(jitter.y));
                ++seenZ.at(static_cast<std::size_t>(jitter.z));
            }
    for (std::size_t v = 0; v < static_cast<std::size_t>(stratum::aquifer::kJitterBoundX); ++v)
        CHECK(seenX.at(v) > 0);
    for (std::size_t v = 0; v < static_cast<std::size_t>(stratum::aquifer::kJitterBoundY); ++v)
        CHECK(seenY.at(v) > 0);
    for (std::size_t v = 0; v < static_cast<std::size_t>(stratum::aquifer::kJitterBoundZ); ++v)
        CHECK(seenZ.at(v) > 0);
    CHECK(seenY.at(9) == 0); // nine values vertically, so 9 never appears
    CHECK(seenX.at(10) == 0);
    CHECK(seenZ.at(10) == 0);
}

TEST_CASE("the centre is the cell corner plus its jitter", "[aquifer]") {
    const stratum::aquifer::CentreSource source{42};
    const auto jitter = source.jitterOf(3, -4, 5);
    const auto centre = source.centreOf(3, -4, 5);
    CHECK(centre.x == (3 * stratum::aquifer::kCellPitchX) + jitter.x);
    CHECK(centre.y == (-4 * stratum::aquifer::kCellPitchY) + jitter.y);
    CHECK(centre.z == (5 * stratum::aquifer::kCellPitchZ) + jitter.z);

    // Every centre lies inside its own cell, which is what makes the grid a
    // partition rather than an overlapping mess.
    CHECK(stratum::aquifer::cellOf(centre.x, centre.y, centre.z) ==
          stratum::aquifer::CellIndex{3, -4, 5});
}

// ---------------------------------------------------------------------------
// The fluid-level decision, and the ocean branch in particular.
//
// The vectors below are the server's own crossings. At each depth the gate is
// dry at the exact crossing floodedness and wet a ten-thousandth above it, and
// each of those pairs is a pair of probe dimensions that differ in nothing but
// that ten-thousandth. Between them they pin both slopes, the depth at which
// the bonus vanishes, and the strictness — in one set of assertions.
// ---------------------------------------------------------------------------

namespace {
using stratum::aquifer::CellFluid;
using stratum::aquifer::cellFluidLevel;
using stratum::aquifer::kLavaLevel;

constexpr std::int32_t kSea = 63;

// An ocean-branch cell at a given depth below the preliminary surface. The
// surface is put well under `sea_level - 8` so the branch is live, and the
// centre is then placed to give exactly that depth.
CellFluid oceanCellAt(const std::int32_t depth, const double floodedness,
                      const std::int32_t preliminarySurface = 0) {
    CellFluid cell;
    cell.preliminarySurface = preliminarySurface;
    cell.centreY = preliminarySurface - depth;
    cell.seaLevel = kSea;
    cell.floodedness = floodedness;
    cell.spread = 0.0;
    return cell;
}

// The two crossings, as exact rationals. A cell takes the sea when
// 11*d < 640*f + 104 and the ladder when 3*d < 160*f + 104, so the crossing
// floodedness is where those hold with equality.
constexpr double seaCrossing(const std::int32_t depth) {
    return static_cast<double>((11 * depth) - 104) / 640.0;
}

constexpr double localCrossing(const std::int32_t depth) {
    return static_cast<double>((3 * depth) - 104) / 160.0;
}
} // namespace

TEST_CASE("the ocean branch's sea gate crosses where the server crossed", "[aquifer]") {
    // Seven depths measured on seeds 3141593 and 777777 at preliminary
    // surfaces 39, 67 and 76, plus the two endpoints. A wrong slope moves
    // every one of them; a slope shared with the ladder gate moves all but
    // the endpoint.
    // The surface sits at 40 so that even the deepest of these keeps its centre
    // above the lava sea, where the deep-cell guard would otherwise mask the
    // gate being tested.
    for (const std::int32_t depth : {4, 8, 10, 12, 30, 36, 42, 48, 56}) {
        const double crossing = seaCrossing(depth);
        INFO("sea gate at depth " << depth);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing, 40)) != kSea);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing + 1e-4, 40)) == kSea);
    }
}

TEST_CASE("the ocean branch's ladder gate crosses on a different slope", "[aquifer]") {
    // The whole refutation of the single-K reading is that these do not sit on
    // the line above. Depth 12 is the negative-operand case: at a preliminary
    // surface of -24 the surface, the centre, the depth's operands and the
    // floorDiv inside the ladder are all negative at once.
    for (const std::int32_t depth : {6, 8, 12, 16, 20, 24, 28, 32, 37, 48, 56}) {
        const double crossing = localCrossing(depth);
        INFO("ladder gate at depth " << depth);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing, 40)) == kLavaLevel);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing + 1e-4, 40)) != kLavaLevel);
    }

    // The negative-operand cases, per §5. At a preliminary surface of -24 the
    // surface, the centre, both operands of the depth and the floorDiv inside
    // the ladder are all negative at once — the shape a truncating division
    // gets wrong. The depths stop at 16 because past that the centre drops
    // onto the lattice step below, whose ladder clamps to the lava sea and so
    // stops distinguishing the two outcomes.
    for (const std::int32_t depth : {6, 8, 12, 16}) {
        const double crossing = localCrossing(depth);
        INFO("ladder gate at depth " << depth << ", everything negative");
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing, -24)) == kLavaLevel);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing + 1e-4, -24)) != kLavaLevel);
    }
}

TEST_CASE("the two gates do not share a slope", "[aquifer]") {
    // Non-parametric, and it assumes nothing about the shape of the bonus. Any
    // ONE bonus added before two fixed thresholds forces the gap between the
    // two crossings to be constant in depth. The server's gap is not constant:
    // it runs 0.4750, 0.4500, 0.4125 and 0.4000 at these depths. No value of
    // K, and no non-linear bonus either, can produce that.
    const auto gapIs = [](std::int32_t depth, double expected) {
        return std::bit_cast<std::uint64_t>(seaCrossing(depth) - localCrossing(depth)) ==
               std::bit_cast<std::uint64_t>(expected);
    };
    CHECK(gapIs(8, 0.4750));
    CHECK(gapIs(24, 0.4500));
    CHECK(gapIs(48, 0.4125));
    CHECK(gapIs(56, 0.4000));
    // At and past the clamp both bonuses are gone, so what remains is the bare
    // distance between the two thresholds — which is where the gap was
    // heading, and where a single bonus would have held it all along.
    CHECK(gapIs(56,
                stratum::aquifer::kFloodedSeaThreshold - stratum::aquifer::kFloodedLocalThreshold));
}

TEST_CASE("the near-surface rule floods whatever the floodedness says", "[aquifer]") {
    // Floodedness -2.0 is two full below the lower gate. Depths 0 to 3 are
    // still the sea; depth 4 is not.
    for (const std::int32_t depth : {0, 1, 2, 3}) {
        INFO("depth " << depth);
        CHECK(cellFluidLevel(oceanCellAt(depth, -2.0)) == kSea);
    }
    CHECK(cellFluidLevel(oceanCellAt(4, -2.0)) != kSea);
}

TEST_CASE("the ocean branch's bonus is clamped at zero, not extrapolated", "[aquifer]") {
    // A floodedness inside the ladder band keeps the ladder out to depth 160.
    // An unclamped bonus would go negative and turn these to lava past about
    // depth 61, which is what the clamp exists to prevent.
    // The centres are chosen to keep the ladder at -20, which is neither the
    // lava sea nor the sea level, so all three outcomes are distinguishable.
    // Depth 160 needs a surface of 120, and so a sea level high enough to keep
    // the ocean branch live under it.
    const auto ladderAt = [](std::int32_t centre, std::int32_t surface, std::int32_t sea) {
        CellFluid cell;
        cell.centreY = centre;
        cell.preliminarySurface = surface;
        cell.seaLevel = sea;
        cell.floodedness = 0.5;
        return cellFluidLevel(cell);
    };
    CHECK(ladderAt(-16, 40, kSea) == -20); // depth 56, exactly at the clamp
    CHECK(ladderAt(-21, 40, kSea) == -20); // depth 61, where an unclamped
    CHECK(ladderAt(-40, 40, kSea) == -20); // depth 80, bonus has gone negative
    CHECK(ladderAt(-40, 120, 200) == -20); // depth 160, and still the ladder

    // And past the clamp the branch reduces exactly to the plain gates.
    CHECK(cellFluidLevel(oceanCellAt(56, 0.85, 40)) == kSea);
    CHECK(cellFluidLevel(oceanCellAt(56, 0.4, 40)) == kLavaLevel);
}

TEST_CASE("the ocean branch runs strictly below sea_level minus eight", "[aquifer]") {
    // The boundary moves one-for-one with sea_level rather than sitting at an
    // absolute height: measured at sea 32, 40, 63 and 100. Depth 20 is deep
    // enough that the two branches disagree.
    const auto flooded = [](std::int32_t preliminarySurface, std::int32_t sea) {
        CellFluid cell;
        cell.preliminarySurface = preliminarySurface;
        cell.centreY = preliminarySurface - 20;
        cell.seaLevel = sea;
        cell.floodedness = 0.6;
        return cellFluidLevel(cell) == sea;
    };
    for (const std::int32_t sea : {32, 40, 63, 100, 200}) {
        INFO("sea level " << sea);
        CHECK(flooded(sea - 9, sea));       // on the branch, and the bonus carries it
        CHECK_FALSE(flooded(sea - 8, sea)); // off it, and 0.6 is below 0.8
    }
}

TEST_CASE("the depth tracks the preliminary surface, not the height", "[aquifer]") {
    // The same depth gives the same crossing forty blocks apart.
    for (const std::int32_t depth : {30, 42}) {
        const double crossing = seaCrossing(depth);
        INFO("depth " << depth);
        for (const std::int32_t surface : {30, -10}) {
            CHECK(cellFluidLevel(oceanCellAt(depth, crossing, surface)) != kSea);
            CHECK(cellFluidLevel(oceanCellAt(depth, crossing + 1e-4, surface)) == kSea);
        }
    }
}

TEST_CASE("a cell below the lava sea reaches the sea only near the surface", "[aquifer]") {
    // The one place the two candidate placements of this guard differ, and the
    // reason it sits after the near-surface rule rather than before it: at a
    // preliminary surface of -54, cells centred at -55, -56 and -57 were
    // observed taking the sea at floodedness -2.0 and at 0.95 alike.
    for (const std::int32_t centre : {-55, -56, -57}) {
        INFO("centre " << centre);
        CellFluid cell;
        cell.preliminarySurface = -54;
        cell.centreY = centre;
        cell.seaLevel = kSea;
        cell.floodedness = -2.0;
        CHECK(cellFluidLevel(cell) == kSea);
        cell.floodedness = 0.95;
        CHECK(cellFluidLevel(cell) == kSea);
    }
    // Through the gates, though, it never reaches the sea: the same centre put
    // well below the surface takes the ladder instead.
    CellFluid deep;
    deep.preliminarySurface = -20;
    deep.centreY = -60;
    deep.seaLevel = kSea;
    deep.floodedness = 0.95;
    CHECK(cellFluidLevel(deep) != kSea);
}

TEST_CASE("the preliminary surface caps the ladder after the spread moves it", "[aquifer]") {
    // Measured: a cell whose lattice point plus offset came to 69 under a
    // surface of 67 was observed at 67, so the cap is applied last.
    CHECK(stratum::aquifer::ladderLevel(50, 67, 0.9) == 67);
    CHECK(stratum::aquifer::ladderLevel(50, 200, 0.9) == 69);
    // Capping first and then adding the offset gives 69 for the first of
    // those, which is two blocks above the surface the server was given and
    // two above where the server put it.
    CHECK(stratum::aquifer::baseLevel(50, 67) + stratum::aquifer::spreadOffset(0.9) == 69);
    CHECK(stratum::aquifer::ladderLevel(50, 67, 0.9) != 69);
    // And the ladder never sinks below the lava sea.
    CHECK(stratum::aquifer::ladderLevel(-200, -100, 0.0) == kLavaLevel);
}
