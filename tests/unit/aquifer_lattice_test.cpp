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
    cell.surface = stratum::aquifer::constantSurface(preliminarySurface);
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
        cell.surface = stratum::aquifer::constantSurface(surface);
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
        cell.surface = stratum::aquifer::constantSurface(preliminarySurface);
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
        cell.surface = stratum::aquifer::constantSurface(-54);
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
    deep.surface = stratum::aquifer::constantSurface(-20);
    deep.centreY = -60;
    deep.seaLevel = kSea;
    deep.floodedness = 0.95;
    CHECK(cellFluidLevel(deep) != kSea);
}

TEST_CASE("the preliminary surface caps the ladder after the spread moves it", "[aquifer]") {
    // Measured: a cell whose lattice point plus offset came to 69 under a
    // surface of 67 was observed at 67, so the cap is applied last.
    CHECK(stratum::aquifer::ladderLevel(50, 67, 0.9, kSea) == 67);
    CHECK(stratum::aquifer::ladderLevel(50, 200, 0.9, kSea) == 69);
    // Capping first and then adding the offset gives 69 for the first of
    // those, which is two blocks above the surface the server was given and
    // two above where the server put it.
    CHECK(stratum::aquifer::baseLevel(50, 67) + stratum::aquifer::spreadOffset(0.9) == 69);
    CHECK(stratum::aquifer::ladderLevel(50, 67, 0.9, kSea) != 69);
    // And the ladder never sinks below the lava sea.
    CHECK(stratum::aquifer::ladderLevel(-200, -100, 0.0, kSea) == kLavaLevel);
}

// ---------------------------------------------------------------------------
// The ocean branch's four psl consumers, and the two places the earlier
// version of this code was wrong.
//
// Everything below is stated against a stubbed `PslRead`, so it needs no world
// and no noise. The four values only ever diverge when the surface varies in
// space, which is exactly why ~1370 probe dimensions could measure the slopes
// correctly and never notice there were four of them.
// ---------------------------------------------------------------------------

namespace {
using stratum::aquifer::constantSurface;
using stratum::aquifer::lambdaLevel;
using stratum::aquifer::PslRead;

CellFluid cellWith(const PslRead surface, const std::int32_t seaLevel, const std::int32_t centreY,
                   const double floodedness, const double spread = 0.0) {
    CellFluid cell;
    cell.surface = surface;
    cell.seaLevel = seaLevel;
    cell.centreY = centreY;
    cell.floodedness = floodedness;
    cell.spread = spread;
    return cell;
}
} // namespace

TEST_CASE("the bonus is a product over a divisor, not a pre-divided constant", "[aquifer]") {
    // The two cells that refute the committed spelling. `fl(11/640)` rounds UP
    // by 1.39e-18, and over the reachable range that is enough to fire the sea
    // gate where the server does not — at exactly two reaches, 45 and 50, on
    // 48 cells across thirteen dimensions and three seeds.
    //
    // Reach 45 is depth 11 and reach 50 is depth 6, which are the only two
    // reaches in [0, 52] where the spellings can differ, so these two cases
    // plus a control are complete coverage of the defect.
    const auto seaGateFires = [](double floodedness, std::int32_t depth) {
        const std::int32_t seaLevel = 240;
        const std::int32_t surface = 150;
        return cellFluidLevel(cellWith(constantSurface(surface), seaLevel, surface - depth,
                                       floodedness)) == seaLevel;
    };
    CHECK_FALSE(seaGateFires(0.0265625, 11)); // committed spelling said sea; server is dry
    CHECK_FALSE(seaGateFires(-0.059375, 6));  // likewise
    // The control: a hair higher and it does fire, so the cases above are not
    // simply testing a gate that never fires.
    CHECK(seaGateFires(0.0265626, 11));
    CHECK(seaGateFires(-0.0593749, 6));

    // The arithmetic itself, so a future edit that reintroduces the
    // pre-divided constant fails here rather than in a golden.
    CHECK_FALSE(0.0265625 + ((45 * 11.0) / 640.0) > 0.8); // the server's answer
    CHECK(0.0265625 + (45 * (11.0 / 640.0)) > 0.8);       // what the old spelling gave
    CHECK_FALSE(-0.059375 + ((50 * 11.0) / 640.0) > 0.8);
    CHECK(-0.059375 + (50 * (11.0 / 640.0)) > 0.8);
    // And the spelling `reach / 640.0 * 11.0`, refuted on the server at
    // floodedness -0.425 and depth 12.
    CHECK(-0.425 + (44 / 160.0 * 3.0) > 0.4);
    CHECK_FALSE(-0.425 + ((44 * 3.0) / 160.0) > 0.4);
}

TEST_CASE("every level below the lava sea moves with sea_level", "[aquifer]") {
    // Λ. Invisible until somebody mapped the global fluid picker with aquifers
    // off: below min(-54, sea_level) the world is lava whatever the aquifer
    // says, so no block readout can separate any level at or below it, and
    // four campaigns read the lava sea's top and recorded it as a level.
    CHECK(lambdaLevel(200) == -54);
    CHECK(lambdaLevel(63) == -54);
    CHECK(lambdaLevel(-54) == -54);
    CHECK(lambdaLevel(-56) == -56);
    CHECK(lambdaLevel(-100) == -100);

    // The third outcome is Λ, measured at sea_level -56 where -54 sits two
    // blocks ABOVE the floor and the distinction becomes visible at last.
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 0.3)) == -56);
    // The ladder's lower clamp is Λ too: at sea -100 with a surface of -70,
    // cells read -70 through a band where a bare -54 would have cut them off.
    CHECK(cellFluidLevel(cellWith(constantSurface(-70), -100, -60, 0.6)) == -70);
    CHECK(stratum::aquifer::ladderLevel(-60, -70, 0.0, -100) == -70);
}

TEST_CASE("the trailing guard tests the branch, not the number", "[aquifer]") {
    // The one place those two readings part, and it is the configuration the
    // guard was measured in. At sea_level -56 the third outcome is ALSO -56,
    // so a numeric `level == sea_level` test would floor it to -54 — and the
    // server does not. Same world, same cells, same surface: floodedness 1.0
    // reads -54 and 0.3 reads -56.
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 1.0, 1.0)) == -54);
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 0.3, 1.0)) == -56);
    // The threshold is Λ rather than -54: a cell centred AT Λ is not below it.
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -56, 1.0, 1.0)) == -56);
    // And the replacement is the literal lava level, which here is ABOVE Λ.
    CHECK(stratum::aquifer::kLavaLevel > lambdaLevel(-56));
}

TEST_CASE("the near-surface path is an early return that the guard cannot reach", "[aquifer]") {
    // Two purpose-built campaigns put cells centred below the lava level wet
    // to the top of their territory. An assignment rather than a return would
    // have floored every one of them.
    CHECK(cellFluidLevel(cellWith(constantSurface(-60), 200, -58, 1.0)) == 200);
    // Depth 3 takes the sea whatever the floodedness says; depth 4 does not.
    CHECK(cellFluidLevel(cellWith(constantSurface(150), 200, 147, -2.0)) == 200);
    CHECK(cellFluidLevel(cellWith(constantSurface(150), 200, 146, -2.0)) == lambdaLevel(200));
}

TEST_CASE("an aborting scan refuses the sea outcome", "[aquifer]") {
    // The whole explanation of a failure this project first read as the slopes
    // being wrong in the middle of the floodedness band. A cell whose scan
    // aborted cannot take the sea through a floodedness gate — and the
    // committed code, which had no such term, predicted sea for most of them.
    const PslRead aborted{.gate = 100, .cap = -70, .anchor = 100, .aborted = true};
    CHECK(cellFluidLevel(cellWith(aborted, 200, 0, 0.9)) == -54);
    // Without the abort the same cell floods.
    const PslRead quiet{.gate = 100, .cap = 100, .anchor = 100, .aborted = false};
    CHECK(cellFluidLevel(cellWith(quiet, 200, 0, 0.9)) == 200);

    // But an aborting cell near the surface still floods, if it sits more than
    // twenty blocks clear of the scan's own low sample. The offset is exactly
    // 20: nineteen and twenty-one each lose more than forty cells.
    const PslRead low{.gate = -70, .cap = -70, .anchor = -70, .aborted = true};
    CHECK(cellFluidLevel(cellWith(low, 200, -49, 0.0)) == 200);
    CHECK(cellFluidLevel(cellWith(low, 200, -50, 0.0)) == -54);
    CHECK(cellFluidLevel(cellWith(low, 200, -45, 0.0)) == 200);
    CHECK(cellFluidLevel(cellWith(low, 200, -55, 0.0)) == -54);
    // And the near-surface flip still sits at gate - 4.
    CHECK(cellFluidLevel(cellWith(aborted, 200, 97, 0.9)) == 200);
}

TEST_CASE("the depth path gates on the anchor while the rest gate on the minimum", "[aquifer]") {
    // The asymmetry that produced the failure, and the only load-bearing
    // finding here resting on one instrument. The two readings separate only
    // where sea_level - 8 falls between the anchor and the window minimum,
    // which no campaign had until one went looking.
    //
    // Anchor 100, minimum 40, sea 68 so the threshold is 60: the anchor is
    // above it and the minimum below. The centre is deep enough that the
    // near-surface rule cannot pre-empt the question.
    const PslRead split{.gate = 40, .cap = 40, .anchor = 100, .aborted = false};
    CHECK(cellFluidLevel(cellWith(split, 68, 20, 0.6)) == 20);
    // Gating on the minimum instead would enter the depth path, where a reach
    // of 36 carries floodedness 0.6 well past the sea gate.
    CHECK(0.6 + ((36 * 11.0) / 640.0) > 0.8);

    // The control that isolates it: the SAME sea level and the same cell, with
    // gate and cap identical, differing in the anchor alone. When the anchor
    // agrees with the minimum both readings enter the depth path and the cell
    // floods; when it does not, only the minimum-gated reading would, and the
    // server keeps the ladder.
    const PslRead agreed{.gate = 40, .cap = 40, .anchor = 40, .aborted = false};
    CHECK(cellFluidLevel(cellWith(agreed, 68, 20, 0.6)) == 68);
    CHECK(cellFluidLevel(cellWith(split, 68, 20, 0.6)) == 20);
}

TEST_CASE("the ladder's cap reads the scan's own minimum", "[aquifer]") {
    // The cap reads `cap`, not `gate`: the two other candidates score 0.90 and
    // 0.87, and the server backs `cap` on half the cells where they differ.
    const PslRead split{.gate = 150, .cap = 60, .anchor = 150, .aborted = false};
    CHECK(stratum::aquifer::ladderLevel(90, split.cap, 0.0, 240) == 60);
    CHECK(stratum::aquifer::ladderLevel(90, split.gate, 0.0, 240) == 100);

    // The reach clamps at zero, so a cell far below the surface still takes
    // the ladder rather than turning to lava. Spread 3.0 offsets it by 30.
    CHECK(cellFluidLevel(cellWith(constantSurface(150), 240, 90, 0.5, 3.0)) == 130);
    CHECK(stratum::aquifer::spreadOffset(3.0) == 30);
}
