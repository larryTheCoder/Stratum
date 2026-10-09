// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The vectors below are not invented: each is a level the vanilla server put
// in a chunk when the probe held `fluid_level_spread` at that constant, read
// off the water-to-air boundary in a world with no terrain in it. The base of
// -20 is the offset that configuration produced.
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using stratum::aquifer::spreadOffset;

namespace {
constexpr std::int32_t kMeasuredBase = -20;

/// The level a cell at the measured base takes for @p spread: centred in
/// the band whose lattice point is -20, under a surface too high to cap it.
[[nodiscard]] std::int32_t fluidLevel(const std::int32_t base, const double spread) {
    REQUIRE(base == kMeasuredBase);
    return stratum::aquifer::ladderLevel(/*centreY=*/-20, /*cap=*/96, spread);
}
} // namespace

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

    // The index is taken on SHIFTED coordinates — (-5, +1, -5) — so the x
    // boundary sits at x = 5 (mod 16) rather than at 0. Each of these was
    // computed by hand from `floorDiv(x - 5, 16)` and its siblings.
    CHECK(cellOf(11, -1, 11) == CellIndex{0, 0, 0});
    CHECK(cellOf(5, 11, 5) == CellIndex{0, 1, 0});
    CHECK(cellOf(4, 11, 4) == CellIndex{-1, 1, -1}); // one block west of the seam
    CHECK(cellOf(21, 12, 21) == CellIndex{1, 1, 1});

    // The cases a truncating division gets wrong. The shift makes these more
    // dangerous rather than less: a coordinate that was safely positive can be
    // negative once shifted, so a `/` here is wrong for x in [0, 5) too.
    CHECK(cellOf(-1, -2, -1) == CellIndex{-1, -1, -1});
    CHECK(cellOf(-6, 0, -22) == CellIndex{-1, 0, -2});
    CHECK(cellOf(0, 0, 0) == CellIndex{-1, 0, -1});
    CHECK(cellOf(-11, -14, -11) == CellIndex{-1, -2, -1});
    CHECK(cellOf(-12, -12, -12) == CellIndex{-2, -1, -2});

    // The world floor of vanilla's overworld, which is not a multiple of 12.
    CHECK(cellOf(0, -64, 0).y == -6);
    CHECK(cellOf(0, -59, 0).y == -5);
}

TEST_CASE("the base sits on its own lattice, capped by the surface", "[aquifer]") {
    // The ladder with the spread at zero: its lattice point, capped.
    const auto baseLevel = [](std::int32_t y, std::int32_t surface) {
        return stratum::aquifer::ladderLevel(y, surface, 0.0);
    };

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

    // There is no floor under the lattice: `the preliminary surface caps
    // the ladder after the spread moves it` pins it sinking below the lava.
}

TEST_CASE("the centre jitter draws ten, nine and ten", "[aquifer]") {
    using stratum::aquifer::CentreSource;
    using stratum::aquifer::Jitter;

    // Known answers from the derivation recovered against the server. They are
    // self-consistent by construction; what ties them to vanilla is the
    // conformance case, which scores this same code on the server's own blocks
    // and runs in CI's conformance job on the comb worlds CI generates. In
    // this binary, deepslate_aquifer_oracle_test.cpp checks the base and the
    // mix's y term against an independent implementation; the x and z terms,
    // the bounds and the draw order have only these answers here, and the
    // server there.
    const CentreSource s42{42, stratum::density::RandomSource::Xoroshiro};
    CHECK(s42.jitterOf(0, 0, 0) == Jitter{7, 7, 9});
    CHECK(s42.jitterOf(0, -4, 0) == Jitter{9, 4, 6});
    CHECK(s42.jitterOf(3, -4, 5) == Jitter{6, 7, 8});
    CHECK(s42.jitterOf(-1, 1, -1) == Jitter{0, 6, 4});
    CHECK(s42.jitterOf(7, 7, 7) == Jitter{2, 1, 8});

    const CentreSource s7{7, stratum::density::RandomSource::Xoroshiro};
    CHECK(s7.jitterOf(0, 0, 0) == Jitter{9, 6, 8});
    CHECK(s7.jitterOf(3, -4, 5) == Jitter{0, 6, 2});

    // A negative seed, since the position mix sign-extends and shifts
    // arithmetically, and every step of it wraps.
    const CentreSource sneg{-1, stratum::density::RandomSource::Xoroshiro};
    CHECK(sneg.jitterOf(0, 0, 0) == Jitter{2, 7, 4});
    CHECK(sneg.jitterOf(7, 7, 7) == Jitter{8, 8, 5});
}

TEST_CASE("under the legacy source the centre jitter is java.util.Random's", "[aquifer][legacy]") {
    using stratum::aquifer::CentreSource;
    using stratum::aquifer::Jitter;
    using stratum::density::RandomSource;

    // Known answers from a JVM, not from this build: java.util.Random and
    // String.hashCode run in jshell over the clean-room spec's position mix
    // (Q3.4), composed the way the server's legacy aquifer worlds measured it
    // (SPEC §11, "The aquifer under the legacy source").
    CHECK(stratum::rng::javaStringHashCode("minecraft:aquifer") == -1973797502);
    CHECK(stratum::rng::javaStringHashCode("minecraft:bedrock_floor") == 2042456806);
    CHECK(stratum::rng::javaStringHashCode("") == 0);
    CHECK(stratum::rng::legacyPositionalSourceFor(42, "minecraft:aquifer").seed() ==
          -651457591642403070);
    CHECK(stratum::rng::legacyPositionalSourceFor(-1, "minecraft:aquifer").seed() ==
          -95934913821622040);
    CHECK(stratum::rng::legacyPositionalSourceFor(31337, "minecraft:aquifer").seed() ==
          8565364769789071309);

    const CentreSource s42{42, RandomSource::Legacy};
    CHECK(s42.source() == RandomSource::Legacy);
    CHECK(s42.legacySeed() == -651457591642403070);
    CHECK(s42.jitterOf(0, 0, 0) == Jitter{2, 2, 4});
    CHECK(s42.jitterOf(0, -4, 0) == Jitter{9, 1, 0});
    CHECK(s42.jitterOf(3, -4, 5) == Jitter{5, 0, 5});
    CHECK(s42.jitterOf(-1, 1, -1) == Jitter{7, 2, 5});
    CHECK(s42.jitterOf(7, 7, 7) == Jitter{5, 0, 3});

    const CentreSource sneg{-1, RandomSource::Legacy};
    CHECK(sneg.jitterOf(0, 0, 0) == Jitter{2, 7, 1});
    CHECK(sneg.jitterOf(7, 7, 7) == Jitter{6, 7, 3});
    CHECK(sneg.jitterOf(-3, -5, 2) == Jitter{4, 1, 7});

    // The same composition spelled out with the generator itself, over a
    // block of cells with negative indices in every axis: the stream seed
    // XOR the mix, then java.util.Random's own nextInt(10), (9), (10).
    for (std::int32_t cx = -3; cx <= 3; ++cx) {
        for (std::int32_t cy = -6; cy <= 6; ++cy) {
            for (std::int32_t cz = -3; cz <= 3; ++cz) {
                stratum::rng::JavaRandom random{s42.legacySeed() ^
                                                stratum::rng::positionSeed(cx, cy, cz)};
                const std::int32_t u = random.nextInt(10);
                const std::int32_t v = random.nextInt(9);
                const std::int32_t w = random.nextInt(10);
                CAPTURE(cx, cy, cz);
                REQUIRE(s42.jitterOf(cx, cy, cz) == Jitter{u, v, w});
            }
        }
    }
}

TEST_CASE("the legacy lattice sees 48 bits of the world seed and the modern one sees 64",
          "[aquifer][legacy]") {
    using stratum::aquifer::CentreSource;
    using stratum::density::RandomSource;

    // java.util.Random keeps the low 48 bits of its seed, so 42 and 42 with the
    // sign bit set are one world to the legacy lattice; Xoroshiro128++ is
    // seeded from all 64. The server shows both on the probe's twin world
    // (vanilla_aquifer_legacy_test.cpp); this pins the derivation's half.
    constexpr std::int64_t kTwin = 42 ^ std::numeric_limits<std::int64_t>::min();
    CHECK(CentreSource{42, RandomSource::Legacy}.legacySeed() ==
          CentreSource{kTwin, RandomSource::Legacy}.legacySeed());
    CHECK_FALSE(CentreSource{42, RandomSource::Xoroshiro}.base() ==
                CentreSource{kTwin, RandomSource::Xoroshiro}.base());

    // And the two sources are different lattices at one seed: of 1000 cells,
    // the draws agree on all three axes at about the chance rate, 1 in 900.
    const CentreSource legacy{42, RandomSource::Legacy};
    const CentreSource modern{42, RandomSource::Xoroshiro};
    CHECK(legacy.base() == stratum::rng::Seed128{});
    CHECK(modern.legacySeed() == 0);
    int same = 0;
    for (std::int32_t cx = 0; cx < 10; ++cx) {
        for (std::int32_t cy = 0; cy < 10; ++cy) {
            for (std::int32_t cz = 0; cz < 10; ++cz) {
                same +=
                    static_cast<int>(legacy.jitterOf(cx, cy, cz) == modern.jitterOf(cx, cy, cz));
            }
        }
    }
    CHECK(same < 10);
}

TEST_CASE("the jitter fills its per-axis bounds and no more", "[aquifer]") {
    // Ten horizontally, nine vertically — not one width on every axis, which is
    // what the measurement first suggested. Over 32000 cells every value inside
    // each bound appears and nothing outside it does.
    const stratum::aquifer::CentreSource source{42, stratum::density::RandomSource::Xoroshiro};
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
    const stratum::aquifer::CentreSource source{42, stratum::density::RandomSource::Xoroshiro};
    const auto jitter = source.jitterOf(3, -4, 5);
    const auto centre = source.centreOf(3, -4, 5);
    CHECK(centre.x == (3 * stratum::aquifer::kCellPitchX) + jitter.x);
    CHECK(centre.y == (-4 * stratum::aquifer::kCellPitchY) + jitter.y);
    CHECK(centre.z == (5 * stratum::aquifer::kCellPitchZ) + jitter.z);

    // A centre does NOT map back to its own cell, and that is the shift doing
    // its job rather than a defect. `cellOf` names the HOME cell of the
    // candidate window; the window runs forward only in x and z, so the centre
    // of cell i lands in home cell i - 1 or i, and cell i is a candidate from
    // either. What must hold is that the cell is always reachable.
    const auto home = stratum::aquifer::cellOf(centre.x, centre.y, centre.z);
    CHECK(home.x >= 2);
    CHECK(home.x <= 3);
    CHECK(home.z >= 4);
    CHECK(home.z <= 5);
    CHECK(home.y >= -5);
    CHECK(home.y <= -4);
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
using stratum::aquifer::kNeverLevel;

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
    // The dry side reads `kNeverLevel`, not `kLavaLevel`. These lines used to
    // pin lambda, which at `sea_level` 63 IS the lava level — and no block
    // readout could ever have told the two apart, Q2.4 handing every row
    // below lambda to the global lava sea (lattice.hpp's `kNeverLevel`). What
    // the gate's CROSSING is remains exactly as measured; only the level on
    // the dry side of it moves.
    for (const std::int32_t depth : {6, 8, 12, 16, 20, 24, 28, 32, 37, 48, 56}) {
        const double crossing = localCrossing(depth);
        INFO("ladder gate at depth " << depth);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing, 40)) == kNeverLevel);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing + 1e-4, 40)) != kNeverLevel);
    }

    // The negative-operand cases, per §5. At a preliminary surface of -24 the
    // surface, the centre, both operands of the depth and the floorDiv inside
    // the ladder are all negative at once — the shape a truncating division
    // gets wrong. These depths used to stop at 16: past that the centre drops
    // onto the lattice step below, whose ladder the old lower clamp pulled up
    // to the lava sea, which is where the dry outcome also sat, so the two
    // stopped being distinguishable. With the clamp gone the ladder stays at
    // -60 there and the whole range discriminates again.
    for (const std::int32_t depth : {6, 8, 12, 16, 20, 24, 28, 32, 37, 48, 56}) {
        const double crossing = localCrossing(depth);
        INFO("ladder gate at depth " << depth << ", everything negative");
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing, -24)) == kNeverLevel);
        CHECK(cellFluidLevel(oceanCellAt(depth, crossing + 1e-4, -24)) != kNeverLevel);
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
    CHECK(cellFluidLevel(oceanCellAt(56, 0.4, 40)) == kNeverLevel);
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
    CHECK(stratum::aquifer::ladderLevel(50, 67, 0.9) == 67);
    CHECK(stratum::aquifer::ladderLevel(50, 200, 0.9) == 69);
    // Capping first and then adding the offset gives 69 for the first of
    // those, which is two blocks above the surface the server was given and
    // two above where the server put it.
    CHECK(std::min(stratum::aquifer::ladderLevel(50, 200, 0.0), 67) +
              stratum::aquifer::spreadOffset(0.9) ==
          69);
    CHECK(stratum::aquifer::ladderLevel(50, 67, 0.9) != 69);
    // And the ladder DOES sink below the lava sea — this line used to assert
    // the opposite, pinning a `max(lambda, ...)` that Q5.7 does not have.
    // What the campaigns behind that clamp measured was its VALUE (lambda
    // against a bare -54); its EXISTENCE was never measurable from a block,
    // Q2.4 owning every row below lambda. Π separates them: unclamping cuts
    // the pooled mixed/pure barrier misses over rows lambda-1..+40 from
    // 704/1790 to 677/1063 on its own, and the `barrier3way` world's
    // three-source misses from 121 of 11 923 real barriers to 6 once the dry
    // sentinel lands with it. 40 * floorDiv(-200, 40) + 20 = -180, under the
    // cap of -100, so -180 is the answer with no floor in the way.
    CHECK(stratum::aquifer::ladderLevel(-200, -100, 0.0) == -180);
    CHECK(stratum::aquifer::ladderLevel(-200, -100, 0.0) < kLavaLevel);
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
using stratum::aquifer::CellIndex;
using stratum::aquifer::constantSurface;
using stratum::aquifer::ladderLevel;
using stratum::aquifer::lambdaLevel;
using stratum::aquifer::PslRead;
using stratum::aquifer::readPreliminarySurface;

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

    // The sentinel a DRY source reports, and its DERIVATION rather than its
    // literal: Q1.4 composes it from four separately-read definitions, and
    // the spec's own open question 4 says to recompute rather than trust the
    // transcribed number. What the corpus measures is only that the dry
    // level sits at least 32 below lambda — sweeping it as `lambda - K` over
    // the three water/lava worlds, K = 32, 64, 256 and this sentinel score
    // byte-identically while K = 16 does not — so the value itself is
    // arithmetic. lattice.hpp carries Q1.4's derivation; these are the
    // spec's transcribed products, checked against it at compile time.
    STATIC_REQUIRE(stratum::aquifer::detail::kHorizontalBits == 26);
    STATIC_REQUIRE(stratum::aquifer::detail::kYBits == 12);
    STATIC_REQUIRE(stratum::aquifer::detail::kYSpan == 4064);
    STATIC_REQUIRE(stratum::aquifer::detail::kYUpper == 2031);
    STATIC_REQUIRE(stratum::aquifer::kMinYLimit == -2032);
    STATIC_REQUIRE(kNeverLevel == -32512);
    CHECK(kNeverLevel < lambdaLevel(-100) - 32);

    // The third outcome is AT OR BELOW Λ, and that bound is what the
    // `sea_level` -56 campaign actually established — on 9740 discriminating
    // cells, where a bare -54 is right on 8729 of them. It separated -54 from
    // Λ; it could not separate Λ from anything lower, because the rows below
    // Λ are Q2.4's and paint identically whatever level sits there. The
    // sentinel is consistent with every block that campaign measured, and it
    // is Π, not a readout, that picked it out (lattice.hpp's `kNeverLevel`).
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 0.3)) == kNeverLevel);
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 0.3)) <= lambdaLevel(-56));
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 0.3)) != -54);
    // The ladder's LEVEL below the lava sea is still what that campaign
    // measured — at sea -100 with a surface of -70, cells read -70 through a
    // band where a bare -54 would have cut them off. Only the clamp that used
    // to sit under it is gone, and it never bound here.
    CHECK(cellFluidLevel(cellWith(constantSurface(-70), -100, -60, 0.6)) == -70);
    CHECK(stratum::aquifer::ladderLevel(-60, -70, 0.0) == -70);
}

TEST_CASE("the trailing guard tests the branch, not the number", "[aquifer]") {
    // The one place those two readings part, and it is the configuration the
    // guard was measured in. At sea_level -56 the SEA outcome is also -56, so
    // a numeric `level == sea_level` test would floor it to -54 — and the
    // server does not. Same world, same cells, same surface: floodedness 1.0
    // reads -54 and 0.3 does not.
    //
    // It is the FIRST of these two lines that pins the guard, and it is
    // unchanged. The second used to read -56 because the dry outcome was
    // lambda; it now reads the sentinel, which is equally "not -54" and makes
    // the same point — the guard did not fire on the dry cell then either,
    // its `tookSea` being false.
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 1.0, 1.0)) == -54);
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -58, 0.3, 1.0)) == kNeverLevel);
    // The threshold is Λ rather than -54: a cell centred AT Λ is not below it.
    CHECK(cellFluidLevel(cellWith(constantSurface(-30), -56, -56, 1.0, 1.0)) == -56);
    // And the replacement is the literal lava level, which here is ABOVE Λ.
    CHECK(stratum::aquifer::kLavaLevel > lambdaLevel(-56));
}

TEST_CASE("the aborting near-surface floor is A_lava, -54 and lava at every sea level",
          "[aquifer]") {
    // Measured at `sea_level` -70 and -60, where lambda and the compile
    // constant `kLavaLevel`(-54) part company: every block whose nearest
    // cell takes this floor is lava up to y = -55 or barrier stone (lowsea's
    // a_lo, 135 716 blocks; aquifer-lowfloor-probe.sh's lf_f60) — spec
    // Q1.1's A_lava, which Q5.3(b) hands an aborted scan. Pipeline engine v9
    // and earlier floored it at lambda, dry above the lava sea, and typed it
    // as the cell's own fluid.
    using stratum::aquifer::cellLevel;
    using stratum::aquifer::LevelOrigin;
    using stratum::aquifer::sourceStatus;
    const PslRead deeplyAborting = readPreliminarySurface(
        [](std::int32_t, std::int32_t, std::int32_t) { return -200.0; }, CellIndex{0, 0, 0}, -70);
    REQUIRE(deeplyAborting.aborted);

    // Comfortably above lambda(-70): still takes the sea, same as at ordinary
    // sea levels where this project's earlier vectors already pinned it.
    CHECK(cellFluidLevel(cellWith(deeplyAborting, -70, -50, 0.5)) == -70);
    // Below lambda: the literal -54, above lambda(-70) — not lambda, which
    // this returned through engine v9. Every centreY in the range agrees.
    for (const std::int32_t centreY : {-100, -190}) {
        const auto floor = cellLevel(cellWith(deeplyAborting, -70, centreY, 0.5));
        CHECK(floor.level == stratum::aquifer::kLavaLevel);
        CHECK(floor.origin == LevelOrigin::GlobalLava);
    }
    // Lava whatever the cell's own type would be: centred at or above lambda
    // with `lava` 0.0 the cell's own fluid is water (a_lo's cells centred
    // -70..-65, which the server fills with lava).
    const PslRead nearCap = readPreliminarySurface(
        [](std::int32_t, std::int32_t, std::int32_t) { return -85.0; }, CellIndex{0, 0, 0}, -70);
    REQUIRE(nearCap.aborted);
    const auto ownWater = sourceStatus(cellWith(nearCap, -70, -66, 0.9), 0.0);
    CHECK(ownWater.level == stratum::aquifer::kLavaLevel);
    CHECK(ownWater.type == stratum::aquifer::FluidType::Lava);

    // At an ORDINARY sea level the level is unchanged: lambda(63) ==
    // kLavaLevel(-54), so nothing the barrier campaigns verified there moves.
    // Only the type does, and no consumer can see it there (the substance
    // case "the floor's type reaches nothing at a sea at or above -54").
    const PslRead ordinaryAborting = readPreliminarySurface(
        [](std::int32_t, std::int32_t, std::int32_t) { return -200.0; }, CellIndex{0, 0, 0}, 63);
    CHECK(cellFluidLevel(cellWith(ordinaryAborting, 63, -100, 0.5)) ==
          stratum::aquifer::kLavaLevel);
    // Centred at -52, between lambda and cap + 20 (-50): water as the
    // cell's own, lava as A_lava.
    const PslRead ordinaryNearSurface = readPreliminarySurface(
        [](std::int32_t, std::int32_t, std::int32_t) { return -70.0; }, CellIndex{0, 0, 0}, 63);
    REQUIRE(ordinaryNearSurface.aborted);
    CHECK(sourceStatus(cellWith(ordinaryNearSurface, 63, -52, 0.5), 0.0).type ==
          stratum::aquifer::FluidType::Lava);

    // The `cap + 20` exemption itself is UNTOUCHED by this fix — this probe
    // never varied it, and the comparand is still `cap`. Checked at an
    // ORDINARY sea level, where lambda(63) == kLavaLevel(-54) and the two
    // outcomes are numerically distinct, so the exemption's own boundary
    // stays observable rather than collapsing the way it does at -70.
    const PslRead ordinaryNearCap = readPreliminarySurface(
        [](std::int32_t, std::int32_t, std::int32_t) { return -65.0; }, CellIndex{0, 0, 0}, 63);
    REQUIRE(ordinaryNearCap.aborted);
    REQUIRE(ordinaryNearCap.cap == -65);                                  // cap+20 = -45
    CHECK(cellFluidLevel(cellWith(ordinaryNearCap, 63, -40, 0.5)) == 63); // > cap+20: sea
    CHECK(cellFluidLevel(cellWith(ordinaryNearCap, 63, -50, 0.5)) ==
          stratum::aquifer::kLavaLevel); // >= lambda but <= cap+20: floor
}

TEST_CASE("the near-surface floor also reads the scan's cap, not its gate", "[aquifer]") {
    // MA blocker 2's own remaining mark on this comparand, closed rather than
    // left at "0 of 2067 cells" (aquifer_lattice.cpp's own comment): every
    // earlier probe that could even abort here held psl CONSTANT, which
    // makes `gate` and `cap` the same number by construction
    // (readPreliminarySurface, sampling.hpp) and so could never separate
    // them from THIS comparand specifically — as opposed to the ladder's own
    // read of `cap`, settled separately above ("the ladder's cap reads the
    // scan's own minimum").
    //
    // `tools/analysis/aquifer-nearsurface-probe.sh` drives a genuinely
    // varying psl and reads block-by-block rather than by fluid body (a
    // body-boundary reading is corrupted here by water/lava contact turning
    // to obsidian mid-column, which a first version of the analyzer learned
    // the hard way): `cap` scores a perfect 1.0000 against `gate`'s
    // 0.9351-0.9358 on the 7.6-7.8M blocks the subset's sources own, per
    // seed of two, and on the 287 237 sampled blocks where the two readings
    // place different blocks the server holds `cap`'s on every one
    // (vanilla_aquifer_nearsurface_test.cpp).
    const PslRead split{.gate = -20, .cap = -70, .anchor = -20, .aborted = true};
    // gate - centreY = 2, inside the near-surface window. cap+20 = -50,
    // gate+20 = 0: a centreY of -22 sits strictly between them, so the two
    // readings disagree outright — cap says sea, gate would say floor.
    CHECK(cellFluidLevel(cellWith(split, 63, -22, 0.5)) == 63);
}

TEST_CASE("the near-surface path is an early return that the guard cannot reach", "[aquifer]") {
    // Two purpose-built campaigns put cells centred below the lava level wet
    // to the top of their territory. An assignment rather than a return would
    // have floored every one of them.
    CHECK(cellFluidLevel(cellWith(constantSurface(-60), 200, -58, 1.0)) == 200);
    // Depth 3 takes the sea whatever the floodedness says; depth 4 does not.
    CHECK(cellFluidLevel(cellWith(constantSurface(150), 200, 147, -2.0)) == 200);
    // Depth 4 is off the near-surface path and this floodedness floods
    // nothing, so the dry outcome is the sentinel. It used to read
    // `lambdaLevel(200)`; what the case tests is the depth-3/depth-4
    // boundary, which is untouched.
    CHECK(cellFluidLevel(cellWith(constantSurface(150), 200, 146, -2.0)) == kNeverLevel);
}

TEST_CASE("both short-circuit seas carry the near-surface origin", "[aquifer]") {
    // The near-surface return and an aborted scan's sea are the global
    // picker's status, typed by neither the centre nor the `lava` override
    // (fluid_type.hpp); the origin is how `sourceStatus` tells. Measured on
    // aquifer-fluidnear-probe.sh's worlds at sea -20.
    using stratum::aquifer::cellLevel;
    using stratum::aquifer::LevelOrigin;
    // Non-aborted, centred within the near-surface window.
    const auto nearSea = cellLevel(cellWith(constantSurface(-40), -20, -30, -2.0));
    CHECK(nearSea.level == -20);
    CHECK(nearSea.origin == LevelOrigin::NearSurfaceSea);
    // Aborted (psl -64 under the -62 threshold), centred more than twenty
    // above the scan's minimum: the sea, from the same early return.
    const PslRead aborting{.gate = -64, .cap = -64, .anchor = -64, .aborted = true};
    const auto abortedSea = cellLevel(cellWith(aborting, -20, -40, -2.0));
    CHECK(abortedSea.level == -20);
    CHECK(abortedSea.origin == LevelOrigin::NearSurfaceSea);
    // Twenty or fewer above it: the floor, the global picker's other status
    // (A_lava; the case on the floor above).
    const auto floored = cellLevel(cellWith(aborting, -20, -44, -2.0));
    CHECK(floored.level == stratum::aquifer::kLavaLevel);
    CHECK(floored.origin == LevelOrigin::GlobalLava);
    // And the cell's own sea branch is the cell's.
    const auto ownSea = cellLevel(cellWith(constantSurface(96), -20, -30, 0.9));
    CHECK(ownSea.level == -20);
    CHECK(ownSea.origin == LevelOrigin::Cell);
}

TEST_CASE("an aborting scan refuses the sea outcome", "[aquifer]") {
    // The whole explanation of a failure this project first read as the slopes
    // being wrong in the middle of the floodedness band. A cell whose scan
    // aborted cannot take the sea through a floodedness gate — and the
    // committed code, which had no such term, predicted sea for most of them.
    //
    // MA blocker 2's own remaining mark on this guard, closed: every earlier
    // probe that could exercise it held psl CONSTANT, so `aborted` was never
    // true while floodedness alone would otherwise cross the gate in a world
    // that could also vary. `aquifer-nearsurface-probe.sh` builds that
    // configuration directly and reads block-by-block: refusing the sea
    // scores 0.9992-0.9996 against 0.066-0.089 for ignoring the abort with
    // the anchor below `sea_level - 8`, and 0.9979-0.9987 against
    // 0.645-0.651 at or above it, per seed of two on a frozen corpus, every
    // block it misses fluid that moved (vanilla_aquifer_nearsurface_test.cpp;
    // the unfrozen corpus's 0.9911-0.9941 and 0.9812-0.9829 were that flow).
    const PslRead aborted{.gate = 100, .cap = -70, .anchor = 100, .aborted = true};
    // The cell is refused the sea and takes A_lava, -54 — lambda at this sea
    // (see the case on that floor below). What this case tests is that the
    // abort refuses the sea at all, which the contrast with `quiet`
    // establishes; the anchor (100) is below the ocean gate (192), so this is
    // the depth path, and the next case has the other branch.
    CHECK(cellFluidLevel(cellWith(aborted, 200, 0, 0.9)) == lambdaLevel(200));
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

TEST_CASE("an aborted scan is refused the sea with its anchor at or above sea_level - 8",
          "[aquifer]") {
    // The other branch of the same refusal. Before pipeline engine v11 each
    // branch of the level rule carried its own `!aborted` guard, and nothing
    // reached this one aborted while floodedness alone would have granted the
    // sea; since then one early return refuses both, and this pins it here
    // too, on a read the scan itself produces rather than one written out.
    // One window sample at -70 (below the -62 abort) and 96 everywhere else:
    // the anchor and the gate stay 96, at or above the ocean gate (55), and
    // the whole window's minimum is -70.
    const auto oneLowSample = [](const std::int32_t x, std::int32_t, const std::int32_t z) {
        return (x == 16 && z == 16) ? -70.0 : 96.0;
    };
    const PslRead read = readPreliminarySurface(oneLowSample, CellIndex{0, 0, 0}, 63);
    REQUIRE(read == PslRead{.gate = 96, .cap = -70, .anchor = 96, .aborted = true});
    REQUIRE(read.anchor >= 63 - stratum::aquifer::kOceanGateOffset);
    // Floodedness 0.9 clears the sea gate on this branch; the abort refuses
    // it, and the cell takes A_lava.
    const auto refused = stratum::aquifer::cellLevel(cellWith(read, 63, 0, 0.9));
    CHECK(refused.level == stratum::aquifer::kLavaLevel);
    CHECK(refused.origin == stratum::aquifer::LevelOrigin::GlobalLava);
    // The same window without the low sample floods, and the gate is strict.
    const PslRead quiet = readPreliminarySurface(
        [](std::int32_t, std::int32_t, std::int32_t) { return 96.0; }, CellIndex{0, 0, 0}, 63);
    REQUIRE(quiet == constantSurface(96));
    CHECK(cellFluidLevel(cellWith(quiet, 63, 0, 0.9)) == 63);
    CHECK(cellFluidLevel(cellWith(quiet, 63, 0, 0.8)) != 63);
}

TEST_CASE("the depth path gates on the anchor while the rest gate on the minimum", "[aquifer]") {
    // MA blocker 1, closed: a second, independent instrument now confirms
    // this rather than resting on the one that found it. A probe with two
    // large psl regions (100 and 40, sized well past the scan window's own
    // 48-block reach) puts many cells' ANCHOR in the high region while their
    // WINDOW pokes into the low one -- anchor and gate differing by 60, eight
    // times the required margin -- and reads each cell at its own centre,
    // strictly inside its own ~12-block territory rather than across a wide
    // band that mostly belongs to other cells. 202 of 210 genuinely
    // discriminating cells, across eight floodedness values and dozens of
    // distinct geometries, match anchor-gating; the eight exceptions are two
    // specific cells whose ladder level exactly equals their own centreY --
    // a boundary tie in the readout, not a rival pattern.
    //
    // The two readings separate only where sea_level - 8 falls between the
    // anchor and the window minimum, which no campaign had until one went
    // looking.
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
    CHECK(stratum::aquifer::ladderLevel(90, split.cap, 0.0) == 60);
    CHECK(stratum::aquifer::ladderLevel(90, split.gate, 0.0) == 100);

    // The reach clamps at zero, so a cell far below the surface still takes
    // the ladder rather than turning to lava. Spread 3.0 offsets it by 30.
    CHECK(cellFluidLevel(cellWith(constantSurface(150), 240, 90, 0.5, 3.0)) == 130);
    CHECK(stratum::aquifer::spreadOffset(3.0) == 30);
}

TEST_CASE("y_skip steps every twelve blocks and floors a negative surface", "[aquifer]") {
    using stratum::aquifer::ySkip;
    // Spec Q2.5: 12 * (floordiv(S_max + 8 + 12, 12) + 1) + 10.
    CHECK(ySkip(96) == 130);
    CHECK(ySkip(-20) == 22);
    CHECK(ySkip(-9) == 22); // the top of the same twelve-block step
    CHECK(ySkip(-8) == 34);
    // A negative dividend: floorDiv(-1, 12) is -1, where truncation would
    // give 0 and put the skip a whole step too high.
    CHECK(ySkip(-21) == 10);
    CHECK(ySkip(-80) == -38);
    CHECK(ySkip(-92) == -50);
    CHECK(ySkip(-93) == -62);
    // A datapack's psl saturates at the int range (javamath::floorToInt);
    // the arithmetic wraps as Java's int does. Evaluated at compile time,
    // where undefined behaviour would not compile.
    STATIC_REQUIRE(ySkip(std::numeric_limits<std::int32_t>::max()) == -2147483618);
    STATIC_REQUIRE(ySkip(std::numeric_limits<std::int32_t>::min()) == -2147483606);
}

TEST_CASE("y_skip's rectangle is where the chunk's lattice extent puts source centres",
          "[aquifer]") {
    using stratum::aquifer::ySkipRectangle;
    // Spec Q3.5: i from floorDiv(xMin - 5, 16) to floorDiv(xMax - 5, 16) + 1;
    // the rectangle runs from the first cell's origin, 16 * i, to the last
    // cell's origin plus the jitter's reach, + 9 (measured: SPEC §11). The
    // shift of 5 puts the low edge one cell below the chunk on BOTH signs of
    // coordinate, which is where a truncating division would part company
    // with floorDiv.
    const auto origin = ySkipRectangle(0, 0);
    CHECK(origin.minX == -16);
    CHECK(origin.maxX == 25);
    CHECK(origin.minZ == -16);
    CHECK(origin.maxZ == 25);

    const auto east = ySkipRectangle(16, 32);
    CHECK(east.minX == 0);
    CHECK(east.maxX == 41);
    CHECK(east.minZ == 16);
    CHECK(east.maxZ == 57);

    const auto west = ySkipRectangle(-16, -32);
    CHECK(west.minX == -32);
    CHECK(west.maxX == 9);
    CHECK(west.minZ == -48);
    CHECK(west.maxZ == -7);

    // Inclusive of both endpoints at a stride of four from the low corner:
    // eleven samples a side, the last at +24.
    CHECK((origin.maxX - origin.minX) / stratum::aquifer::kYSkipSampleStride + 1 == 11);
}

TEST_CASE("a chunk's y_skip reads its whole rectangle at y = 0 and no other column", "[aquifer]") {
    using stratum::aquifer::chunkYSkip;
    // A flat -92 everywhere, and one column raised to -80: the step from -50
    // to -38 says whether that column is read.
    const auto spike = [](std::int32_t atX, std::int32_t atZ) {
        return [atX, atZ](std::int32_t x, std::int32_t y, std::int32_t z) {
            CHECK(y == stratum::aquifer::kPreliminarySurfaceSampleY);
            return (x == atX && z == atZ) ? -80.0 : -92.0;
        };
    };
    CHECK(chunkYSkip(spike(1000, 1000), 0, 0) == -50);
    // Both corners of the rectangle, and its far sample row.
    CHECK(chunkYSkip(spike(-16, -16), 0, 0) == -38);
    CHECK(chunkYSkip(spike(24, 24), 0, 0) == -38);
    CHECK(chunkYSkip(spike(24, -16), 0, 0) == -38);
    // One stride past either end, and a column between two samples.
    CHECK(chunkYSkip(spike(-20, 0), 0, 0) == -50);
    CHECK(chunkYSkip(spike(28, 0), 0, 0) == -50);
    CHECK(chunkYSkip(spike(2, 2), 0, 0) == -50);
    // A chunk off the origin, on the negative side: the same offsets.
    CHECK(chunkYSkip(spike(-48 + 24, -48 - 16), -48, -48) == -38);
    CHECK(chunkYSkip(spike(-48 + 28, -48), -48, -48) == -50);

    // Floored, not truncated: -92.5 is -93, a step below -92.
    const auto flat = [](double value) {
        return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
    };
    CHECK(chunkYSkip(flat(-92.0), 0, 0) == -50);
    CHECK(chunkYSkip(flat(-92.5), 0, 0) == -62);
}

TEST_CASE("the lattice is consulted at y_skip itself and not above it", "[aquifer]") {
    using stratum::aquifer::consultsLattice;
    CHECK(consultsLattice(-50, -50));
    CHECK(consultsLattice(-51, -50));
    CHECK_FALSE(consultsLattice(-49, -50));
}

TEST_CASE("the deep-dark override needs both thresholds, both strict", "[aquifer]") {
    using stratum::aquifer::isDeepDark;
    CHECK(isDeepDark(-0.3, 1.0));
    CHECK_FALSE(isDeepDark(-0.225, 1.0)); // erosion AT the threshold is not below it
    CHECK_FALSE(isDeepDark(-0.3, 0.9));   // depth AT the threshold is not above it
    CHECK_FALSE(isDeepDark(0.0, 1.0));
    CHECK_FALSE(isDeepDark(-0.3, 0.0));
    CHECK(isDeepDark(std::nextafter(-0.225, -1.0), std::nextafter(0.9, 2.0)));
}

TEST_CASE("a deep-dark cell is dry, except on the near-surface return", "[aquifer]") {
    // Q5.9 forces both floodedness comparands to -1. The ladder below floods
    // at floodedness 0.5 and would flood the same at 1.0; with the override
    // neither reading clears a threshold, so the cell is the dry sentinel.
    CellFluid ladder = cellWith(constantSurface(150), 240, 90, 0.5, 3.0);
    REQUIRE(cellFluidLevel(ladder) == 130);
    ladder.deepDark = true;
    CHECK(cellFluidLevel(ladder) == kNeverLevel);
    ladder.floodedness = 1.0;
    CHECK(cellFluidLevel(ladder) == kNeverLevel);

    // The ocean branch's sea outcome goes the same way.
    CellFluid sea = cellWith(constantSurface(40), 68, 20, 1.0);
    REQUIRE(cellFluidLevel(sea) == 68);
    sea.deepDark = true;
    CHECK(cellFluidLevel(sea) == kNeverLevel);

    // The near-surface return compares no floodedness, so there is no
    // comparand to force: a deep-dark cell three blocks under the surface
    // still takes the sea.
    CellFluid shallow = cellWith(constantSurface(150), 200, 147, -2.0);
    shallow.deepDark = true;
    CHECK(cellFluidLevel(shallow) == 200);

    // Nor an aborted scan: its lambda status precedes the level rule, and so
    // the override (aquifer-ddfloor-probe.sh; over three seeds the sentinel
    // built 10 577 blocks of barrier the server does not). Wet or dry without
    // the override, it reads lambda with it.
    const PslRead aborted{.gate = -20, .cap = -70, .anchor = -20, .aborted = true};
    for (const double floodedness : {-2.0, 0.6, 0.9}) {
        CellFluid deep = cellWith(aborted, 63, -150, floodedness);
        REQUIRE(cellFluidLevel(deep) == lambdaLevel(63));
        deep.deepDark = true;
        CHECK(cellFluidLevel(deep) == lambdaLevel(63));
    }
}

TEST_CASE("a datapack's NaN or out-of-range router value is defined, as Java's int makes it",
          "[aquifer]") {
    // No vanilla router comes near these; the point is that no input is
    // undefined behaviour. Java's `(int) Math.floor` maps NaN to 0 and
    // saturates out-of-range values, and its int arithmetic wraps.
    using stratum::aquifer::spreadOffset;
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    CHECK(spreadOffset(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(spreadOffset(1e300) == 3 * (kMax / 3)); // 2147483646
    // floorDiv(INT_MIN, 3) * 3 is one below INT_MIN, and wraps to INT_MAX.
    CHECK(spreadOffset(-1e300) == kMax);

    // The scan's three readings saturate, and a NaN field reads 0.
    const CellIndex centre{.x = 10, .y = 0, .z = 10};
    const auto flat = [](double value) {
        return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
    };
    const PslRead huge = readPreliminarySurface(flat(1e300), centre, 63);
    CHECK(huge.gate == kMax);
    CHECK(huge.cap == kMax);
    CHECK(huge.anchor == kMax);
    CHECK_FALSE(huge.aborted);
    const PslRead nan =
        readPreliminarySurface(flat(std::numeric_limits<double>::quiet_NaN()), centre, 63);
    CHECK(nan.gate == 0);
    CHECK(nan.cap == 0);
    CHECK(nan.anchor == 0);
    CHECK_FALSE(nan.aborted);

    // And the level rule's own arithmetic on a saturated reading wraps
    // rather than trapping. Q5.3(a)'s threshold `cap + 20` wraps as Java's
    // int would: under a surface at INT_MAX it is INT_MIN + 19, so a cell
    // centred at 0 sits "more than twenty above" it and takes the sea.
    CHECK(cellFluidLevel(cellWith(huge, 63, 0, 0.5)) == 63);
    // A surface at INT_MIN is an ocean cell, and every cell above
    // INT_MIN + 20 is more than twenty above it: the sea, by Q5.3(a), before
    // the depth path is reached.
    const PslRead floor{.gate = kMin, .cap = kMin, .anchor = kMin, .aborted = false};
    CHECK(cellFluidLevel(cellWith(floor, 63, 0, -1.0)) == 63);
    CHECK(cellFluidLevel(cellWith(floor, 63, 1, -1.0)) == 63);
    // Aborted, the same surface still reaches the depth path: one block
    // above it the depth INT_MIN - 1 wraps to INT_MAX, exactly as Java's
    // would, the reach clamps at zero, and the dry outcome is floored at
    // lambda.
    const PslRead abortedFloor{.gate = kMin, .cap = kMin, .anchor = kMin, .aborted = true};
    CHECK(cellFluidLevel(cellWith(abortedFloor, 63, 1, -1.0)) == lambdaLevel(63));
}

TEST_CASE("a cell more than twenty above a land surface takes the sea, Q5.3(a)", "[aquifer]") {
    using stratum::aquifer::cellLevel;
    using stratum::aquifer::LevelOrigin;
    // Off the ocean branch (surface 87, sea 95: 87 >= 95 - 8), floodedness
    // 0.25 is under both gates, so a cell is dry — unless it is centred more
    // than twenty above the surface, where the global picker's status, the
    // sea, is taken first (aquifer-level-probe.sh, SPEC §11).
    const PslRead land = constantSurface(87);
    CHECK(cellFluidLevel(cellWith(land, 95, 107, 0.25)) == kNeverLevel); // twenty above
    CHECK(cellFluidLevel(cellWith(land, 95, 108, 0.25)) == 95);          // twenty-one
    // Whatever the floodedness would have given: the ladder, at 0.6.
    CHECK(cellFluidLevel(cellWith(land, 95, 107, 0.6)) == 87);
    CHECK(cellFluidLevel(cellWith(land, 95, 108, 0.6)) == 95);
    // It is the global picker's status, typed the default fluid as both
    // near-surface seas are: the lava override does not reach it.
    const auto level = cellLevel(cellWith(land, 95, 108, 0.25));
    CHECK(level.origin == LevelOrigin::NearSurfaceSea);
    // And before Q5.9's override, which forces only the gates' comparands.
    CellFluid deep = cellWith(land, 95, 108, 0.95);
    deep.deepDark = true;
    CHECK(cellFluidLevel(deep) == 95);
    deep.centreY = 107;
    CHECK(cellFluidLevel(deep) == kNeverLevel);
    // The threshold reads the surface, not the sea: with the sea well under
    // the surface (psl 87, sea 65) a cell 21 above the surface takes the sea
    // and one 13 above the sea does not.
    CHECK(cellFluidLevel(cellWith(land, 65, 108, 0.25)) == 65);
    CHECK(cellFluidLevel(cellWith(land, 65, 78, 0.25)) == kNeverLevel);
    // An aborted scan off the near-surface path is left to the abort's own
    // floor (the near-surface probe refuses it the sea).
    const PslRead aborted{.gate = 87, .cap = -70, .anchor = 87, .aborted = true};
    CHECK(cellFluidLevel(cellWith(aborted, 95, 108, 0.25)) == lambdaLevel(95));
}

TEST_CASE("the Q5.6 floodedness clamp is inert", "[aquifer]") {
    // The code does not clamp (lattice.hpp, by the thresholds). This is the
    // proof that it need not: every out-of-range floodedness gives the same
    // level as its clamp, over every depth the ocean branch can see, both
    // gating sides of `sea_level - 8`, and an aborting scan or not.
    const double outside[] = {
        std::nextafter(1.0, 2.0),   1.5,  2.0,  1e6,  std::numeric_limits<double>::infinity(),
        std::nextafter(-1.0, -2.0), -1.5, -2.0, -1e6, -std::numeric_limits<double>::infinity()};
    std::size_t compared = 0;
    for (const double floodedness : outside) {
        const double clamped = std::clamp(floodedness, -1.0, 1.0);
        for (const std::int32_t anchor : {40, 60}) { // sea 68: gate at 60
            for (const bool aborted : {false, true}) {
                for (std::int32_t depth = 0; depth <= 64; ++depth) {
                    const PslRead surface{
                        .gate = 40, .cap = 40, .anchor = anchor, .aborted = aborted};
                    const std::int32_t centreY = 40 - depth;
                    INFO("floodedness " << floodedness << " anchor " << anchor << " aborted "
                                        << aborted << " depth " << depth);
                    CHECK(cellFluidLevel(cellWith(surface, 68, centreY, floodedness)) ==
                          cellFluidLevel(cellWith(surface, 68, centreY, clamped)));
                    ++compared;
                }
            }
        }
    }
    CHECK(compared == 10U * 2U * 2U * 65U);
}

TEST_CASE("an aborted scan reads -54 off the near-surface path, and nothing else is floored",
          "[aquifer]") {
    // At sea 63, measured through the barrier, the only consumer that sees a
    // level below lambda there: over an aborting surface with vanilla's
    // barrier on (aquifer-nsfloor-probe.sh) the unfloored levels build 7 690
    // blocks of stone the server does not, and -54 builds none; on worlds
    // whose scans never reach below lambda (water/lava, deep-floor,
    // capfloor's cf200) the ladder stays unclamped, and a floor there breaks
    // them. At sea 63 -54 is lambda; the case below this one has the seas
    // where it is not.
    const std::int32_t lambda = lambdaLevel(63);
    REQUIRE(lambda == stratum::aquifer::kLavaLevel);
    const PslRead aborted{.gate = -20, .cap = -70, .anchor = -20, .aborted = true};
    // Off the near-surface path (depth 16), refused the sea, ladder capped at
    // -70: floored. The cap binds here, so this alone cannot tell a floor on
    // the abort from a floor on the cap.
    CHECK(cellFluidLevel(cellWith(aborted, 63, -36, 0.9)) == lambda);
    // The cell that can: 130 blocks down, the ladder's own rung (-140) sits
    // below the cap, so raising the cap to lambda would leave it at -140.
    // The abort floors it anyway.
    const std::int32_t rung = ladderLevel(-150, aborted.cap, 0.0);
    REQUIRE(rung < aborted.cap);
    const PslRead notAborted{.gate = -20, .cap = -70, .anchor = -20, .aborted = false};
    CHECK(cellFluidLevel(cellWith(notAborted, 63, -150, 0.6)) == rung);
    CHECK(cellFluidLevel(cellWith(aborted, 63, -150, 0.6)) == lambda);
    // And an aborted cell that nothing floods reads lambda too, not the dry
    // sentinel: at floodedness 0 the server sides with lambda on all 3 770
    // blocks where the two part.
    CHECK(cellFluidLevel(cellWith(aborted, 63, -36, -2.0)) == lambda);
    // So every aborted cell off the near-surface path reads exactly -54,
    // whatever its floodedness and spread: it takes A_lava before the level
    // rule (spec Q5.3(b)).
    for (std::int32_t centreY = -200; centreY <= -24; centreY += 7) {
        for (const double floodedness : {-2.0, 0.0, 0.3, 0.5, 0.9, 2.0}) {
            for (const double spread : {-1.0, 0.0, 1.0}) {
                CHECK(cellFluidLevel(cellWith(aborted, 63, centreY, floodedness, spread)) ==
                      lambda);
            }
        }
    }

    // Without the abort a cap below lambda is NOT floored: psl -58 sits below
    // lambda without reaching the -62 that aborts the scan. This is the
    // documented reading (spec Q5.3(b) ties the A_lava status to the abort),
    // and capfloor's cf58l arm bears it out, thinly: on the 4 blocks over two
    // seeds where it and a floor on the cap part, the server takes this one
    // (vanilla_aquifer_nsfloor_test.cpp).
    // A cell deep enough that no sea bonus reaches it takes the ladder.
    const PslRead low{.gate = -58, .cap = -58, .anchor = -58, .aborted = false};
    const std::int32_t unfloored = cellFluidLevel(cellWith(low, 63, -150, 0.6));
    CHECK(unfloored < lambda);
    CHECK(unfloored != kNeverLevel);
    // ... and its dry outcome stays the sentinel.
    CHECK(cellFluidLevel(cellWith(low, 63, -150, -2.0)) == kNeverLevel);
}

TEST_CASE("below a sea under -54 an aborted scan reads -54 and lava, deep-dark or not",
          "[aquifer]") {
    // Where lambda is `sea_level` the reading engines v5 and v6 took (lambda)
    // and A_lava part in blocks: at sea -70 an aborted cell off the
    // near-surface path holds lava to y = -55, with Q5.9's override and
    // without (aquifer-lowfloor-probe.sh's lf_v5, lf_v5w and lf_dd).
    using stratum::aquifer::cellLevel;
    using stratum::aquifer::FluidType;
    using stratum::aquifer::LevelOrigin;
    using stratum::aquifer::sourceStatus;
    // Anchor -20 clears the sea's -78 gate; a -88 in the window aborts.
    const PslRead aborted{.gate = -20, .cap = -88, .anchor = -20, .aborted = true};
    for (const bool deepDark : {false, true}) {
        for (const double floodedness : {0.0, 0.6, 0.9}) {
            for (const std::int32_t centreY : {-74, -66, -40, 10}) {
                CellFluid cell = cellWith(aborted, -70, centreY, floodedness);
                cell.deepDark = deepDark;
                INFO("deep dark " << deepDark << ", floodedness " << floodedness << ", centre "
                                  << centreY);
                const auto level = cellLevel(cell);
                CHECK(level.level == stratum::aquifer::kLavaLevel);
                CHECK(level.origin == LevelOrigin::GlobalLava);
                // `lava` 0.0: a cell's own fluid would be water at a centre
                // at or above lambda.
                CHECK(sourceStatus(cell, 0.0).type == FluidType::Lava);
            }
        }
    }
    // The same cells without the abort keep the level rule: the ladder at
    // 0.6 (water, centred above lambda), the dry sentinel under the override.
    const PslRead quiet{.gate = -20, .cap = -20, .anchor = -20, .aborted = false};
    const auto ladder = sourceStatus(cellWith(quiet, -70, -66, 0.6), 0.0);
    CHECK(ladder.level == -60);
    CHECK(ladder.type == FluidType::Default);
    CellFluid dark = cellWith(quiet, -70, -66, 0.9);
    dark.deepDark = true;
    CHECK(cellFluidLevel(dark) == kNeverLevel);
    // And the trailing guard's sea below lambda is the same status.
    const auto guarded = cellLevel(cellWith(quiet, -70, -74, 0.9));
    CHECK(guarded.level == stratum::aquifer::kLavaLevel);
    CHECK(guarded.origin == LevelOrigin::GlobalLava);
}
