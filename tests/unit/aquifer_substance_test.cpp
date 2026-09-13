// Stratum — the aquifer's whole answer for one block, at the lava sea's top.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Two clauses of the substance decision live only at the global lava sea:
// Q2.4 (below it, the sea wins and the lattice is never consulted) and Q6.3
// (on its top row, a nearest source reading water is water outright, with
// no barrier). Both reduce to the global picker of Q1.2, which is a function
// of `y` alone, so both are pinned here by construction rather than read off
// a probe — the SHAPE is what these check. The numbers behind them (which
// blocks the server really writes on that row, and how many barriers the
// bare fall-through would have put there) live in
// `vanilla_aquifer_waterlava_test.cpp` and substance.hpp's own header.
//
// The last case is Q6.4's mixed-type branch driven END TO END: the `lava`
// sampler alone flips a junction inside two fluid bodies from air to stone,
// at a separation where the constant fires and — with both bodies water —
// nothing does (barrier.hpp's own unit cases pin the arithmetic). That is
// the shape that proves every ranked source's TYPE reaches Π, cache and
// all, not just the nearest wet one's.
#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

using stratum::aquifer::AquiferQuery;
using stratum::aquifer::BarrierAt;
using stratum::aquifer::BarrierSource;
using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::computeSubstance;
using stratum::aquifer::FluidType;
using stratum::aquifer::globalReadsLava;
using stratum::aquifer::kLavaLevel;
using stratum::aquifer::lambdaLevel;
using stratum::aquifer::placesBarrier;
using stratum::aquifer::Selection;
using stratum::aquifer::selectSources;
using stratum::aquifer::StatusCache;
using stratum::aquifer::Substance;
using stratum::aquifer::SubstanceAt;
using stratum::aquifer::waterOverLava;

namespace {

constexpr std::int32_t kSea = 63;

/// A block on the sea's top row whose two nearest sources are close enough
/// to compete and both centred ABOVE the sea (so a flooded one reads water
/// there, not lava), and which keeps the same nearest pair one row up — so
/// one configuration can be read on the row the exception owns and on the
/// row it does not.
struct Junction {
    std::int32_t x = 0;
    std::int32_t z = 0;
    CellIndex nearest{};
    CellIndex second{};
};

[[nodiscard]] std::optional<Junction> findJunction(const CentreSource& centres,
                                                   const std::int32_t row) {
    for (std::int32_t z = 0; z < 256; ++z) {
        for (std::int32_t x = 0; x < 256; ++x) {
            const Selection onRow = selectSources(centres, x, row, z);
            const Selection above = selectSources(centres, x, row + 1, z);
            const bool samePair = onRow.ranked[0].centre == above.ranked[0].centre &&
                                  onRow.ranked[1].centre == above.ranked[1].centre;
            const bool bothAboveSea =
                onRow.ranked[0].centre.y >= row && onRow.ranked[1].centre.y >= row;
            // Close enough that D = -1 plus the pressure at a 117-block level
            // gap crosses zero on both rows (barrier.hpp's Q6.4): a
            // separation under 8 leaves s12 above 0.68, comfortably past the
            // 0.43 the arithmetic needs at barrier 0.
            const bool competing = onRow.separation() < 8 && above.separation() < 8;
            // The third source must not be near-tied with the second, or the
            // "flip" case below would not be the clean two-source picture.
            const bool thirdFar = onRow.ranked[2].distanceSq - onRow.ranked[1].distanceSq >= 25 &&
                                  above.ranked[2].distanceSq - above.ranked[1].distanceSq >= 25;
            if (samePair && bothAboveSea && competing && thirdFar) {
                return Junction{.x = x,
                                .z = z,
                                .nearest = onRow.ranked[0].centre,
                                .second = onRow.ranked[1].centre};
            }
        }
    }
    return std::nullopt;
}

/// Floods exactly one centre: the floodedness read AT that centre is past
/// the sea gate (level = sea_level, water on every row below it), every
/// other centre reads below the local gate (level = lambda, air on every
/// row at or above it).
struct FloodOne {
    CellIndex flooded;

    double operator()(std::int32_t x, std::int32_t y, std::int32_t z) const {
        return (x == flooded.x && y == flooded.y && z == flooded.z) ? 0.9 : -1.0;
    }
};

constexpr auto kZero = [](std::int32_t, std::int32_t, std::int32_t) { return 0.0; };
constexpr auto kSurface96 = [](std::int32_t, std::int32_t, std::int32_t) { return 96.0; };

[[nodiscard]] SubstanceAt decide(const CentreSource& centres, const AquiferQuery& query,
                                 const FloodOne& flood) {
    StatusCache cache;
    return computeSubstance(centres, query, cache, kZero, flood, kZero, kZero, kSurface96);
}

/// A block on the row y = -21 whose nearest source is centred in the
/// ladder band [-40, -1) — so with floodedness on the local branch and a
/// spread of 0 its level is exactly -20, and it reads fluid on the row —
/// and whose second source, once flooded past the sea gate (level 63),
/// reads water there too; the two separated by exactly `separation` in
/// squared distance, the third far enough to be inert.
struct MixedJunction {
    std::int32_t x = 0;
    std::int32_t z = 0;
    CellIndex ladder{}; // level -20 once wet on the local branch
    CellIndex sea{};    // level 63 once flooded past the sea gate
};

constexpr std::int32_t kMixedRow = -21;

[[nodiscard]] std::optional<MixedJunction> findMixedJunction(const CentreSource& centres,
                                                             const std::int32_t separation) {
    for (std::int32_t z = 0; z < 512; ++z) {
        for (std::int32_t x = 0; x < 512; ++x) {
            const Selection sel = selectSources(centres, x, kMixedRow, z);
            const CellIndex& nearest = sel.ranked[0].centre;
            const bool ladderBand = nearest.y >= -40 && nearest.y < 0;
            const bool aboveLambda = sel.ranked[1].centre.y >= lambdaLevel(kSea);
            const bool thirdFar = sel.ranked[2].distanceSq - sel.ranked[1].distanceSq >= 25;
            if (ladderBand && aboveLambda && sel.separation() == separation && thirdFar) {
                return MixedJunction{
                    .x = x, .z = z, .ladder = nearest, .sea = sel.ranked[1].centre};
            }
        }
    }
    return std::nullopt;
}

/// Floodedness for exactly two centres: the ladder source on the local
/// branch (0.6: past 0.4, short of 0.8), the sea source past the sea gate
/// (0.9), everything else dry.
struct FloodTwo {
    MixedJunction junction;

    double operator()(std::int32_t x, std::int32_t y, std::int32_t z) const {
        const CellIndex at{.x = x, .y = y, .z = z};
        if (at == junction.ladder) {
            return 0.6;
        }
        if (at == junction.sea) {
            return 0.9;
        }
        return -1.0;
    }
};

/// The decision at the junction under one `lava` constant and one `barrier`
/// constant.
[[nodiscard]] SubstanceAt decideMixed(const CentreSource& centres, const MixedJunction& junction,
                                      const double lava, const double barrier) {
    StatusCache cache;
    const AquiferQuery query{
        .x = junction.x, .y = kMixedRow, .z = junction.z, .density = -1.0, .seaLevel = kSea};
    const auto lavaAt = [lava](std::int32_t, std::int32_t, std::int32_t) { return lava; };
    const auto barrierAt = [barrier](std::int32_t, std::int32_t, std::int32_t) { return barrier; };
    return computeSubstance(centres, query, cache, barrierAt, FloodTwo{junction}, kZero, lavaAt,
                            kSurface96);
}

} // namespace

TEST_CASE("the global picker reads lava strictly below lambda, whatever the sea", "[aquifer]") {
    // Q1.2: `A_lava` below `min(-54, sea_level)`, the default fluid at and
    // above it — so the sea's top row itself is NOT lava.
    CHECK(globalReadsLava(kLavaLevel - 1, kSea));
    CHECK_FALSE(globalReadsLava(kLavaLevel, kSea));
    CHECK_FALSE(globalReadsLava(kLavaLevel + 1, kSea));
    // A sea above -54 does not move the line: 200 and 63 agree.
    CHECK(globalReadsLava(kLavaLevel - 1, 200));
    CHECK_FALSE(globalReadsLava(kLavaLevel, 200));
    // A sea BELOW -54 does: the line is then the sea itself.
    CHECK(globalReadsLava(-71, -70));
    CHECK_FALSE(globalReadsLava(-70, -70));
    CHECK_FALSE(globalReadsLava(-55, -70));
    CHECK_FALSE(globalReadsLava(kLavaLevel, -70));
    CHECK(lambdaLevel(-70) == -70);
}

TEST_CASE("the water-over-lava exception owns exactly one row", "[aquifer]") {
    // The nearest source reads water from -40 down; the exception fires on
    // the sea's top row and nowhere else.
    CHECK(waterOverLava(kLavaLevel, kSea, -40, FluidType::Default));
    CHECK_FALSE(waterOverLava(kLavaLevel + 1, kSea, -40, FluidType::Default));
    CHECK_FALSE(waterOverLava(kLavaLevel + 2, kSea, -40, FluidType::Default));
    // Below the row Q2.4 has already answered; the predicate still reads
    // true there, which is why `computeSubstance` asks Q2.4 first.
    CHECK(waterOverLava(kLavaLevel - 1, kSea, -40, FluidType::Default));

    // Not water: the nearest source reads AIR on the row (level at or below
    // it) ...
    CHECK_FALSE(waterOverLava(kLavaLevel, kSea, kLavaLevel, FluidType::Default));
    CHECK_FALSE(waterOverLava(kLavaLevel, kSea, kLavaLevel - 10, FluidType::Default));
    // ... or reads LAVA there.
    CHECK_FALSE(waterOverLava(kLavaLevel, kSea, -40, FluidType::Lava));

    // The row moves with the sea when the sea is below -54.
    CHECK(waterOverLava(-70, -70, -60, FluidType::Default));
    CHECK_FALSE(waterOverLava(-69, -70, -60, FluidType::Default));
    CHECK_FALSE(waterOverLava(kLavaLevel, -70, -40, FluidType::Default));
}

TEST_CASE("below the lava sea the substance is lava before any source is consulted", "[aquifer]") {
    const CentreSource centres{42};
    const FloodOne nothingFlooded{.flooded = CellIndex{.x = 1 << 20, .y = 0, .z = 0}};
    for (const std::int32_t y : {kLavaLevel - 1, kLavaLevel - 5, -64}) {
        const AquiferQuery query{.x = 7, .y = y, .z = 11, .density = -1.0, .seaLevel = kSea};
        const SubstanceAt at = decide(centres, query, nothingFlooded);
        INFO("y " << y);
        CHECK(at.substance == Substance::Fluid);
        CHECK(at.fluidType == FluidType::Lava);
    }
    // The row itself is the lattice's: nothing flooded, so it reads air.
    const AquiferQuery onRow{.x = 7, .y = kLavaLevel, .z = 11, .density = -1.0, .seaLevel = kSea};
    CHECK(decide(centres, onRow, nothingFlooded).substance == Substance::Air);
}

TEST_CASE("water on the sea's top row is water, one row up it is a barrier", "[aquifer]") {
    const CentreSource centres{42};
    const std::int32_t row = lambdaLevel(kSea);
    const auto junction = findJunction(centres, row);
    REQUIRE(junction.has_value());

    // Flood the NEAREST source: it reads water on both rows, its competitor
    // reads air on both, and they sit close enough that the bare barrier
    // predicate writes stone on both — checked directly, so the case below
    // is known to be discriminating rather than trivially agreeing.
    const FloodOne nearestFlooded{.flooded = junction->nearest};
    for (const std::int32_t y : {row, row + 1}) {
        const Selection sel = selectSources(centres, junction->x, y, junction->z);
        BarrierAt bare;
        bare.y = y;
        bare.density = -1.0;
        bare.nearest = BarrierSource{.level = kSea, .distanceSq = sel.ranked[0].distanceSq};
        bare.second = BarrierSource{.level = row, .distanceSq = sel.ranked[1].distanceSq};
        bare.third = BarrierSource{.level = row, .distanceSq = sel.ranked[2].distanceSq};
        bare.barrier = 0.0;
        INFO("y " << y << " separation " << sel.separation());
        REQUIRE(placesBarrier(bare));
    }

    const AquiferQuery onRow{
        .x = junction->x, .y = row, .z = junction->z, .density = -1.0, .seaLevel = kSea};
    const SubstanceAt atRow = decide(centres, onRow, nearestFlooded);
    CHECK(atRow.substance == Substance::Fluid);
    CHECK(atRow.fluidType == FluidType::Default);

    const AquiferQuery oneUp{
        .x = junction->x, .y = row + 1, .z = junction->z, .density = -1.0, .seaLevel = kSea};
    CHECK(decide(centres, oneUp, nearestFlooded).substance == Substance::Solid);

    // The asymmetry: flood the SECOND source instead, so the nearest reads
    // air on the row. Q6.3 says nothing, and the barrier stands.
    const FloodOne secondFlooded{.flooded = junction->second};
    CHECK(decide(centres, onRow, secondFlooded).substance == Substance::Solid);
    CHECK(decide(centres, oneUp, secondFlooded).substance == Substance::Solid);
}

TEST_CASE("the exception's row follows a sea pulled below the lava level", "[aquifer]") {
    // At sea_level -70 the line is -70: y = -54 is an ordinary row, where a
    // nearest source reading water over a competing air source gets the
    // barrier like anywhere else. (The row -70 itself is not reachable with
    // the constant-floodedness samplers above — a sea-gated source's level
    // IS the sea, -70, which reads air on its own row — so this pins the
    // row that must NOT fire rather than the one that must; the probe in
    // vanilla_aquifer_waterlava_test.cpp reads both off the server.)
    constexpr std::int32_t kLowSea = -70;
    const CentreSource centres{42};
    const auto junction = findJunction(centres, kLavaLevel);
    REQUIRE(junction.has_value());
    const FloodOne nearestFlooded{.flooded = junction->nearest};
    const AquiferQuery at54{
        .x = junction->x, .y = kLavaLevel, .z = junction->z, .density = -1.0, .seaLevel = kLowSea};
    // With the sea at -70 the flooded source's level is -70 — air at -54 —
    // so nothing competes here and this reads air, not stone; what matters
    // is that it is NOT the exception's water.
    const SubstanceAt at = decide(centres, at54, nearestFlooded);
    CHECK(at.substance != Substance::Fluid);
    // And below -70 it is the sea, unconditionally.
    const AquiferQuery below{.x = 3, .y = -71, .z = 3, .density = -1.0, .seaLevel = kLowSea};
    const SubstanceAt sea = decide(centres, below, nearestFlooded);
    CHECK(sea.substance == Substance::Fluid);
    CHECK(sea.fluidType == FluidType::Lava);
}

TEST_CASE("the lava sampler alone turns a junction inside two fluid bodies to stone", "[aquifer]") {
    // Q6.4's mixed-type branch, end to end. The nearest source's level is
    // -20 (the ladder band's base, spread 0) and the second is flooded to
    // the sea (63): at y = -21 BOTH read fluid. With `lava` at 0.0 both are
    // water-typed — one body, no barrier, the block is water. With `lava`
    // at 1.0 the nearest, at level -20 under the ceiling, turns lava-typed
    // while the second stays water at 63: a lava body meeting a water one,
    // and the constant fires at separation 12 (weight 0.52) and not at 13
    // (0.48), whatever the `barrier` noise says.
    const CentreSource centres{42};
    const auto close = findMixedJunction(centres, 12);
    REQUIRE(close.has_value());
    for (const double barrier : {-1.0, 0.0, 4.0}) {
        INFO("barrier " << barrier);
        const SubstanceAt oneBody = decideMixed(centres, *close, 0.0, barrier);
        CHECK(oneBody.substance == Substance::Fluid);
        CHECK(oneBody.fluidType == FluidType::Default);
        CHECK(decideMixed(centres, *close, 1.0, barrier).substance == Substance::Solid);
    }
    const auto apart = findMixedJunction(centres, 13);
    REQUIRE(apart.has_value());
    const SubstanceAt lavaWins = decideMixed(centres, *apart, 1.0, 0.0);
    CHECK(lavaWins.substance == Substance::Fluid);
    CHECK(lavaWins.fluidType == FluidType::Lava);
    CHECK(decideMixed(centres, *apart, 0.0, 0.0).substance == Substance::Fluid);
}
