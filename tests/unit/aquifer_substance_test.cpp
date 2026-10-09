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
#include <stratum/javamath.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

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
    return computeSubstance(centres, query, cache, kZero, flood, kZero, kZero, kSurface96,
                            stratum::aquifer::NoDeepDark{});
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
                            kSurface96, stratum::aquifer::NoDeepDark{});
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
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
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
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
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
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
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
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
    const auto close = findMixedJunction(centres, 12);
    REQUIRE(close.has_value());
    for (const double barrier : {-1.0, 0.0, 4.0}) {
        INFO("barrier " << barrier);
        CHECK(decideMixed(centres, *close, 1.0, barrier).substance == Substance::Solid);
    }
    // The one-body control holds across the `barrier` router's REAL range,
    // which this project measures at about [-1.33, +1.30]. It does NOT hold
    // at the synthetic 4.0 this case used to carry, and that is a
    // measurement rather than a concession: the two levels here are -20 and
    // 63, so at y = -21 the pair reads the same fluid ONE block under the
    // lower level — inside the three-block band the `/3` arm owns, not the
    // `/10` floor below it — and takes that arm with `u = 5/6`. A `barrier`
    // of 4 then pushes Π to 2*(4 + 5/6) and the junction turns to stone.
    // Until that arm was reached at all — `termFires` used to refuse any
    // pair that agreed — this read as "one body, no barrier"; the server
    // says otherwise, on 110 097 250 blocks with no false stone anywhere
    // (barrier.hpp's header). The row below pins the new answer rather than
    // dropping the case.
    for (const double barrier : {-1.0, 0.0}) {
        INFO("barrier " << barrier);
        const SubstanceAt oneBody = decideMixed(centres, *close, 0.0, barrier);
        CHECK(oneBody.substance == Substance::Fluid);
        CHECK(oneBody.fluidType == FluidType::Default);
    }
    CHECK(decideMixed(centres, *close, 0.0, 4.0).substance == Substance::Solid);
    const auto apart = findMixedJunction(centres, 13);
    REQUIRE(apart.has_value());
    const SubstanceAt lavaWins = decideMixed(centres, *apart, 1.0, 0.0);
    CHECK(lavaWins.substance == Substance::Fluid);
    CHECK(lavaWins.fluidType == FluidType::Lava);
    CHECK(decideMixed(centres, *apart, 0.0, 0.0).substance == Substance::Fluid);
}

namespace {
/// A deterministic stand-in for a noise read in [-1, 1): a stub FIELD, not a
/// world RNG, so any fixed mix will do — this one is splitmix64's finaliser.
[[nodiscard]] double stubField(const std::uint64_t salt, const std::int32_t x, const std::int32_t y,
                               const std::int32_t z) {
    std::uint64_t h = salt;
    for (const std::int32_t v : {x, y, z}) {
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(v));
        h += 0x9e3779b97f4a7c15ULL;
        h = (h ^ (h >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        h = (h ^ (h >> 27U)) * 0x94d049bb133111ebULL;
        h ^= h >> 31U;
    }
    return (static_cast<double>(h >> 11U) / 4503599627370496.0) - 1.0; // [-1, 1)
}
} // namespace

namespace {
/// How many blocks in the thirty rows above `ySkip` the full local decision
/// answers differently from the global picker, over a flat
/// `preliminary_surface_level` at @p surface and three stub noise fields.
[[nodiscard]] std::size_t disagreementsAboveSkip(const double surface) {
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
    const std::int32_t skip = stratum::aquifer::ySkip(static_cast<std::int32_t>(surface));
    const auto psl = [surface](std::int32_t, std::int32_t, std::int32_t) { return surface; };
    std::size_t differ = 0;
    for (std::uint64_t salt = 1; salt <= 3; ++salt) {
        const auto field = [salt](std::uint64_t which) {
            return [salt, which](std::int32_t x, std::int32_t y, std::int32_t z) {
                return stubField((salt * 8) + which, x, y, z);
            };
        };
        StatusCache cache;
        for (std::int32_t z = -40; z < 40; z += 3) {
            for (std::int32_t x = -40; x < 40; x += 3) {
                for (std::int32_t y = skip + 1; y <= skip + 30; ++y) {
                    const AquiferQuery query{
                        .x = x, .y = y, .z = z, .density = -1.0, .seaLevel = kSea};
                    const SubstanceAt local =
                        computeSubstance(centres, query, cache, field(0), field(1), field(2),
                                         field(3), psl, stratum::aquifer::NoDeepDark{});
                    const bool globalFluid = y < kSea; // y_skip >= -62 > lambda here
                    const bool agrees = globalFluid ? (local.substance == Substance::Fluid &&
                                                       local.fluidType == FluidType::Default)
                                                    : local.substance == Substance::Air;
                    differ += agrees ? 0 : 1;
                }
            }
        }
    }
    return differ;
}
} // namespace

TEST_CASE("above y_skip the lattice agrees with the global picker down to a surface of -80",
          "[aquifer]") {
    // Why the cutoff is invisible in every vanilla preset, pinned rather than
    // argued: over a flat `preliminary_surface_level` anywhere from 96 down
    // to -80, the full local decision above `ySkip` returns exactly what the
    // global picker does, whatever the four aquifer noises say. `y_skip` is
    // at least S_max + 31, so every candidate centre for a block above it
    // sits clear of the surface and takes the near-surface return — and that
    // holds through the scan's abort (psl below -62), because the aborting
    // floor gives way to the sea for a centre more than twenty blocks above
    // the scan's minimum.
    for (const double surface : {96.0, 63.0, 40.0, -20.0, -61.0, -63.0, -75.0, -80.0}) {
        INFO("surface " << surface);
        CHECK(disagreementsAboveSkip(surface) == 0);
    }
    // One block lower and the step drops `y_skip` from -38 to -50: centres
    // in the band it uncovers sit within twenty blocks of an aborting scan's
    // minimum, take the floor at lambda, and read air where the global
    // picker reads water. So the cutoff is NOT a pure optimisation — a world
    // whose surface sits at -81..-92 shows it, and the server's agrees with
    // it to the row (vanilla_aquifer_yskip_test.cpp) — and ChunkFiller
    // applies it for that reason, not only to save the lattice work.
    CHECK(disagreementsAboveSkip(-81.0) > 0);
    CHECK(disagreementsAboveSkip(-92.0) > 0);
}

namespace {
/// A source's status over a flat surface, at the y_skip probe's constants
/// (floodedness 0.5, spread 0, lava 0).
[[nodiscard]] stratum::aquifer::SourceStatus statusOver(const double surface,
                                                        const std::int32_t centreY) {
    const auto flat = [surface](std::int32_t, std::int32_t, std::int32_t) { return surface; };
    return stratum::aquifer::sourceStatus(
        stratum::aquifer::CellFluid{.centreY = centreY,
                                    .surface = stratum::aquifer::readPreliminarySurface(
                                        flat, CellIndex{.x = 0, .y = centreY, .z = 0}, kSea),
                                    .seaLevel = kSea,
                                    .floodedness = 0.5,
                                    .spread = 0.0},
        0.0);
}
} // namespace

TEST_CASE("under a surface of -75 or lower a source's status is its centre's alone", "[aquifer]") {
    // The y_skip probe's instrument (vanilla_aquifer_yskip_test.cpp), pinned
    // without a server: every scan aborts at its anchor, so a centre at or
    // above lambda takes the sea (it sits more than twenty blocks above the
    // anchor, spec Q5.3(a)) and every centre below it reads no fluid at or
    // above lambda — A_lava where the anchor fires, the level rule on the
    // surface (a ladder at most the surface, or the sentinel) four or more
    // below it. Which blocks are pockets — the nearest source dry — is then
    // the same at every such surface, and only y_skip moves between the
    // probe's dimensions.
    const std::int32_t lambda = lambdaLevel(kSea);
    for (const double surface : {-200.0, -93.0, -92.5, -92.0, -85.0, -81.0, -80.0, -75.0}) {
        INFO("surface " << surface);
        int wrong = 0;
        for (std::int32_t centreY = -130; centreY <= 60; ++centreY) {
            const stratum::aquifer::SourceStatus status = statusOver(surface, centreY);
            const bool fires = centreY > stratum::javamath::floorToInt(surface) - 4;
            const bool expected = centreY >= lambda
                                      ? (status.level == kSea && status.type == FluidType::Default)
                                  : fires ? status.level == lambda
                                          : status.level <= lambda;
            wrong += expected ? 0 : 1;
        }
        CHECK(wrong == 0);
    }
    // And -75 is the bound: one block higher and the centre at lambda itself
    // is no longer twenty blocks clear of the anchor, and reads lambda.
    CHECK(statusOver(-74.0, lambda).level == lambda);
    CHECK(statusOver(-74.0, lambda + 1).level == kSea);
    CHECK(statusOver(-75.0, lambda).level == kSea);
}

namespace {
using stratum::aquifer::FluidExit;
using stratum::aquifer::fluidUpdateFlag;
using stratum::aquifer::SourceStatus;

/// Squared distances whose similarities to the nearest come out as asked:
/// `sim(d1, dj) = 1 - (dj - d1)/25`, so a gap of 25 * (1 - s).
[[nodiscard]] std::array<std::int64_t, 4> distancesFor(std::int64_t gap12, std::int64_t gap13,
                                                       std::int64_t gap14) {
    return {100, 100 + gap12, 100 + gap13, 100 + gap14};
}

constexpr SourceStatus kWaterAt20{.level = 20, .type = FluidType::Default};
constexpr SourceStatus kWaterAt60{.level = 60, .type = FluidType::Default};
constexpr SourceStatus kLavaAt20{.level = 20, .type = FluidType::Lava};
} // namespace

TEST_CASE("the fluid-update flag where Q6.2's nearest source wins", "[aquifer]") {
    // s12 <= 0 (a gap of 25 or more): the nearest pair's difference, but only
    // while the pair is within the flow similarity, -0.76 — a gap of 44.
    // The fourth source is read only by the full path's last clause. Recorded
    // rather than FAILed inside the reader: a reader that cannot return is
    // unreachable code to MSVC, and one that does not is a missing return to
    // GCC.
    bool fourthRead = false;
    const auto never = [&fourthRead] {
        fourthRead = true;
        return SourceStatus{};
    };
    for (const FluidExit exit : {FluidExit::BarrierFellThrough, FluidExit::WaterOverLava}) {
        INFO("exit " << static_cast<int>(exit));
        // Gap 25 (s12 = 0) and gap 44 (s12 = -0.76, exactly the threshold).
        for (const std::int64_t gap : {25, 30, 44}) {
            INFO("gap " << gap);
            CHECK(fluidUpdateFlag(distancesFor(gap, 60, 70), {kWaterAt20, kWaterAt60, kWaterAt60},
                                  exit, never));
            CHECK_FALSE(fluidUpdateFlag(distancesFor(gap, 60, 70),
                                        {kWaterAt20, kWaterAt20, kWaterAt60}, exit, never));
            // A type alone differs too: statuses compare level AND type.
            CHECK(fluidUpdateFlag(distancesFor(gap, 60, 70), {kWaterAt20, kLavaAt20, kWaterAt20},
                                  exit, never));
        }
        // Past the flow similarity the pair is too far apart to matter.
        CHECK_FALSE(fluidUpdateFlag(distancesFor(45, 60, 70), {kWaterAt20, kWaterAt60, kWaterAt60},
                                    exit, never));
    }
    CHECK_FALSE(fourthRead);
}

TEST_CASE("the fluid-update flag on water resting on lava, past Q6.2", "[aquifer]") {
    // Always set — measured, and not in the clean-room spec — whatever the
    // statuses say, all four equal included.
    // The Q6.3 exit reads no fourth source.
    bool fourthRead = false;
    const auto never = [&fourthRead] {
        fourthRead = true;
        return SourceStatus{};
    };
    for (const std::int64_t gap12 : {0, 5, 24}) {
        CHECK(fluidUpdateFlag(distancesFor(gap12, 60, 70), {kWaterAt20, kWaterAt20, kWaterAt20},
                              FluidExit::WaterOverLava, never));
    }
    CHECK_FALSE(fourthRead);
}

TEST_CASE("the fluid-update flag on the full path, the fourth source last", "[aquifer]") {
    int fourthReads = 0;
    const auto fourthIs = [&fourthReads](SourceStatus status) {
        return [&fourthReads, status] {
            ++fourthReads;
            return status;
        };
    };
    const auto flag = [&](std::array<std::int64_t, 4> d, std::array<SourceStatus, 3> s,
                          SourceStatus fourth) {
        return fluidUpdateFlag(d, s, FluidExit::BarrierFellThrough, fourthIs(fourth));
    };
    // The nearest pair differs: set, whatever the rest.
    CHECK(flag(distancesFor(10, 100, 100), {kWaterAt20, kWaterAt60, kWaterAt20}, kWaterAt20));
    // Pair 2-3 within the flow similarity and differing: set. s23 = 1 -
    // (d3 - d2)/25; gap13 54 and gap12 10 give d3 - d2 = 44, exactly -0.76.
    CHECK(flag(distancesFor(10, 54, 100), {kWaterAt20, kWaterAt20, kWaterAt60}, kWaterAt20));
    // ... and just past it, not — while 1-3 is also past it.
    CHECK_FALSE(flag(distancesFor(10, 55, 100), {kWaterAt20, kWaterAt20, kWaterAt60}, kWaterAt20));
    // Pair 1-3 within it and differing: set. (Never ALONE, once 1-2 agree:
    // then 1-3 differ exactly when 2-3 do, and s13 <= s23 always, so the
    // 2-3 clause has fired first. The spec keeps both; so does the code.)
    CHECK(flag(distancesFor(0, 44, 100), {kWaterAt20, kWaterAt20, kWaterAt60}, kWaterAt20));
    CHECK(fourthReads == 0);

    // Only when nothing among the three differs is the fourth read — and
    // only when both 1-3 and 1-4 are within the flow similarity.
    CHECK(flag(distancesFor(0, 10, 44), {kWaterAt20, kWaterAt20, kWaterAt20}, kWaterAt60));
    CHECK(fourthReads == 1);
    CHECK_FALSE(flag(distancesFor(0, 10, 44), {kWaterAt20, kWaterAt20, kWaterAt20}, kWaterAt20));
    CHECK(fourthReads == 2);
    CHECK_FALSE(flag(distancesFor(0, 10, 45), {kWaterAt20, kWaterAt20, kWaterAt20}, kWaterAt60));
    CHECK_FALSE(flag(distancesFor(0, 45, 46), {kWaterAt20, kWaterAt20, kWaterAt20}, kWaterAt60));
    CHECK(fourthReads == 2);
}

namespace {
/// Blocks on rows lambda - 1 .. lambda + 30 where @p rival's status gives a
/// different answer — substance, fluid type or fluid-update flag — from the
/// shipped one, at @p seaLevel, over stub noise fields and a surface that
/// sends cells down every path: aborting (-100), near-surface (-40) and
/// high (96), in blobs two cells wide. @p rival is called with a source's
/// inputs and its shipped status.
template<typename Rival>
[[nodiscard]] std::size_t rivalDisagreements(const std::int32_t seaLevel, Rival&& rival) {
    using stratum::aquifer::RankedCell;
    using stratum::aquifer::Source;
    using stratum::aquifer::SourceStatus;
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
    const std::int32_t lambda = lambdaLevel(seaLevel);
    const auto field = [](std::uint64_t which) {
        return [which](std::int32_t x, std::int32_t y, std::int32_t z) {
            return stubField(which, x, y, z);
        };
    };
    const auto psl = [](std::int32_t x, std::int32_t, std::int32_t z) {
        constexpr std::int32_t kBlob = 32;
        const double pick = stubField(7, stratum::javamath::floorDiv(x, kBlob), 0,
                                      stratum::javamath::floorDiv(z, kBlob));
        return pick < -0.4 ? -100.0 : (pick < 0.2 ? -40.0 : 96.0);
    };
    const stratum::aquifer::NoDeepDark noDeepDark;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, SourceStatus> memo;
    const auto rivalStatus = [&](const Source& ranked) {
        const auto key = std::make_tuple(ranked.centre.x, ranked.centre.y, ranked.centre.z);
        if (const auto found = memo.find(key); found != memo.end()) {
            return found->second;
        }
        const RankedCell inputs = stratum::aquifer::rankedCellOf(ranked, seaLevel, psl, field(1),
                                                                 field(2), field(3), noDeepDark);
        const SourceStatus status =
            rival(inputs, stratum::aquifer::sourceStatus(inputs.cell, inputs.lava));
        memo.emplace(key, status);
        return status;
    };
    StatusCache cache;
    std::size_t differ = 0;
    for (std::int32_t z = -64; z < 64; z += 3) {
        for (std::int32_t x = -64; x < 64; x += 3) {
            for (std::int32_t y = lambda - 1; y <= lambda + 30; ++y) {
                const AquiferQuery query{
                    .x = x, .y = y, .z = z, .density = -1.0, .seaLevel = seaLevel};
                const SubstanceAt shipped = computeSubstance(
                    centres, query, cache, field(0), field(1), field(2), field(3), psl, noDeepDark);
                const SubstanceAt other =
                    stratum::aquifer::computeSubstanceWith(centres, query, rivalStatus, field(0));
                const bool same = shipped.substance == other.substance &&
                                  shipped.fluidType == other.fluidType &&
                                  shipped.fluidUpdate == other.fluidUpdate;
                differ += same ? 0 : 1;
            }
        }
    }
    return differ;
}
} // namespace

TEST_CASE("the floor's type reaches nothing at a sea at or above -54", "[aquifer]") {
    // A_lava's type (lava) against the type engine v9 gave an aborted scan's
    // floor: the cell's own, water for a centre at or above lambda whose
    // `lava` reads 0.3 or less. At a sea at or above -54 the floor's level
    // is lambda itself, so it reads fluid nowhere the lattice is consulted,
    // and every consumer of a type is guarded by a reading: the barrier's
    // mixed-type constant needs both sources reading fluid at the block, and
    // every status the fluid-update flag compares the floor with is, or
    // equals, the nearest source's, which reads fluid there and so sits
    // above lambda — a level difference already. So no block and no mark can
    // depend on it; this pins that rather than argues it.
    const auto ownType = [](const stratum::aquifer::RankedCell& inputs,
                            stratum::aquifer::SourceStatus status) {
        const auto level = stratum::aquifer::cellLevel(inputs.cell);
        if (level.origin == stratum::aquifer::LevelOrigin::GlobalLava) {
            status.type = stratum::aquifer::fluidTypeOf(
                stratum::aquifer::FluidTypeAt{.centreY = inputs.cell.centreY,
                                              .level = level.level,
                                              .seaLevel = inputs.cell.seaLevel,
                                              .lava = inputs.lava});
        }
        return status;
    };
    for (const std::int32_t sea : {63, 0, -40, -54}) {
        INFO("sea " << sea);
        CHECK(rivalDisagreements(sea, ownType) == 0);
    }
    // Not vacuously: below -54 the floor reads fluid from lambda to -55, and
    // the same retyping writes water there where the server holds lava
    // (vanilla_aquifer_lowfloor_test.cpp).
    CHECK(rivalDisagreements(-70, ownType) > 1000);
    // Nor the level: lambda in its place, engine v9's, moves nothing at 63
    // and blocks at -70.
    const auto lambdaFloor = [](const stratum::aquifer::RankedCell& inputs,
                                stratum::aquifer::SourceStatus status) {
        if (stratum::aquifer::cellLevel(inputs.cell).origin ==
            stratum::aquifer::LevelOrigin::GlobalLava) {
            status.level = lambdaLevel(inputs.cell.seaLevel);
        }
        return status;
    };
    CHECK(rivalDisagreements(63, lambdaFloor) == 0);
    CHECK(rivalDisagreements(-70, lambdaFloor) > 1000);
}

// THE TIE-BREAK'S REACH, rank pair by rank pair (spec Q4.7, open question
// 9). Swapping two sources tied in squared distance changes only what
// reads them, and for two of the three pairs Q6.6 and Q8.4 read them
// symmetrically. These sweeps pin that over every combination of a small
// status alphabet, both fluid types, the dry sentinel and the overworld's
// whole density range; golden_aquifer_tiebreak_test.cpp counts the same
// populations on real generation.
namespace {
using stratum::aquifer::kNeverLevel;
using stratum::aquifer::similarity;

/// Levels on both sides of every `y` swept, and the dry sentinel.
constexpr std::array<std::int32_t, 8> kSweepLevels = {-60, -20, -5, 0, 7, 12, 30, kNeverLevel};
constexpr std::array<std::int32_t, 7> kSweepRows = {-30, -6, 0, 6, 10, 20, 40};
/// The overworld's final density at a block the lattice is consulted for
/// lies in [-11/24, 0] (`min(squeeze, noodle)`).
constexpr std::array<double, 6> kSweepDensities = {-11.0 / 24.0, -0.4, -0.3, -0.1, -0.01, 0.0};
constexpr std::array<double, 3> kSweepBarrier = {-1.0, 0.0, 0.7};

[[nodiscard]] std::array<SourceStatus, 16> sweepStatuses() {
    std::array<SourceStatus, 16> out{};
    std::size_t i = 0;
    for (const std::int32_t level : kSweepLevels) {
        for (const FluidType type : {FluidType::Default, FluidType::Lava}) {
            out.at(i++) = SourceStatus{.level = level, .type = type};
        }
    }
    return out;
}

[[nodiscard]] bool same(const SourceStatus& a, const SourceStatus& b) {
    return a.level == b.level && a.type == b.type;
}

[[nodiscard]] BarrierAt barrierOf(std::int32_t y, double density, double noise,
                                  const std::array<SourceStatus, 3>& s,
                                  const std::array<std::int64_t, 3>& d) {
    const auto source = [&](std::size_t r) {
        return BarrierSource{.level = s.at(r).level, .distanceSq = d.at(r), .type = s.at(r).type};
    };
    return BarrierAt{.y = y,
                     .density = density,
                     .nearest = source(0),
                     .second = source(1),
                     .third = source(2),
                     .barrier = noise};
}

/// Calls @p visit with every (y, density, barrier) combination swept.
template<typename Visit>
void forEachBlock(Visit&& visit) {
    for (const std::int32_t y : kSweepRows) {
        for (const double density : kSweepDensities) {
            for (const double noise : kSweepBarrier) {
                visit(y, density, noise);
            }
        }
    }
}
} // namespace

TEST_CASE("a rank 2-3 tie moves the barrier only where all three statuses differ", "[aquifer]") {
    // With d2 = d3, s13 = s12 = s and s23 = 1, so the two orders weigh
    // {s*P12, s^2*P13, s*P23} against {s*P13, s^2*P12, s*P23}. Where A2 = A3
    // they are the same set. Where A1 equals one of them, one pressure is
    // 0 and the other two are equal, and for D <= 0 and 0 < s <= 1 the term
    // `D + s^2*P > 0` implies `D + s*P > 0`, so the s^2 term never decides;
    // the same holds where s = 1 (a three-way tie). So the orders can part
    // only past Q6.2, with d1 < d2, and with A1, A2, A3 pairwise different.
    const auto statuses = sweepStatuses();
    std::size_t parted = 0;
    std::size_t outsideTheLemma = 0;
    for (const SourceStatus& a1 : statuses) {
        for (const SourceStatus& a2 : statuses) {
            for (const SourceStatus& a3 : statuses) {
                const bool allDistinct = !same(a1, a2) && !same(a1, a3) && !same(a2, a3);
                for (const std::int64_t gap : {0, 1, 12, 24}) {
                    const std::array<std::int64_t, 3> d{100, 100 + gap, 100 + gap};
                    forEachBlock([&](std::int32_t y, double density, double noise) {
                        const bool ranked =
                            placesBarrier(barrierOf(y, density, noise, {a1, a2, a3}, d));
                        const bool swapped =
                            placesBarrier(barrierOf(y, density, noise, {a1, a3, a2}, d));
                        if (ranked != swapped) {
                            ++parted;
                            outsideTheLemma += (gap > 0 && allDistinct) ? 0 : 1;
                        }
                    });
                }
            }
        }
    }
    CHECK(outsideTheLemma == 0);
    // And the zero the clean-room spec measured (0 of 792 338 ties) is NOT
    // the predicate's: inside the lemma's population the orders do part.
    INFO("parted " << parted);
    CHECK(parted > 0);
}

TEST_CASE("a rank 1-2 tie never moves the barrier", "[aquifer]") {
    // With d1 = d2, s12 = 1 and s13 = s23, so swapping the nearest pair
    // swaps the second and third terms of Q6.6 and nothing else: the
    // predicate is symmetric. A rank 1-2 tie reaches a block only through
    // the nearest source's own reading — Q6.3's exit, or the fall-through.
    const auto statuses = sweepStatuses();
    std::size_t parted = 0;
    for (const SourceStatus& a1 : statuses) {
        for (const SourceStatus& a2 : statuses) {
            for (const SourceStatus& a3 : statuses) {
                for (const std::int64_t gap13 : {0, 1, 12, 24, 30}) {
                    const std::array<std::int64_t, 3> d{100, 100, 100 + gap13};
                    forEachBlock([&](std::int32_t y, double density, double noise) {
                        const bool ranked =
                            placesBarrier(barrierOf(y, density, noise, {a1, a2, a3}, d));
                        const bool swapped =
                            placesBarrier(barrierOf(y, density, noise, {a2, a1, a3}, d));
                        parted += ranked == swapped ? 0 : 1;
                    });
                }
            }
        }
    }
    CHECK(parted == 0);
}

TEST_CASE("a rank 2-3 tie moves the fluid-update flag only where Q6.2 decides", "[aquifer]") {
    // Past Q6.2 (s12 > 0 > -0.76), Q8.4 with s13 = s12 and s23 = 1 reads
    // "A1 != A2, or A2 != A3, or A1 != A3", then A4 against A1 alone: the
    // same under the swap. Where Q6.2 decides, the flag is A1 != A2 alone,
    // and the swap makes it A1 != A3. So a rank 2-3 tie's flag changes and
    // its substance changes come from DISJOINT populations: the flag only
    // where the nearest source wins outright, the substance only where the
    // barrier is weighed (the case above).
    const auto statuses = sweepStatuses();
    std::size_t partedShortCircuit = 0;
    std::size_t partedPast = 0;
    for (const SourceStatus& a1 : statuses) {
        for (const SourceStatus& a2 : statuses) {
            for (const SourceStatus& a3 : statuses) {
                for (const SourceStatus& a4 : {statuses.front(), statuses.back()}) {
                    const auto fourth = [a4] { return a4; };
                    for (const std::int64_t gap : {0, 1, 24, 25, 30, 44, 45}) {
                        for (const std::int64_t gap4 : {0, 44, 45}) {
                            const std::array<std::int64_t, 4> d{100, 100 + gap, 100 + gap,
                                                                100 + gap + gap4};
                            for (const FluidExit exit :
                                 {FluidExit::BarrierFellThrough, FluidExit::WaterOverLava}) {
                                const bool ranked = fluidUpdateFlag(d, {a1, a2, a3}, exit, fourth);
                                const bool swapped = fluidUpdateFlag(d, {a1, a3, a2}, exit, fourth);
                                if (ranked == swapped) {
                                    continue;
                                }
                                if (similarity(d[0], d[1]) <= 0.0) {
                                    ++partedShortCircuit;
                                } else {
                                    ++partedPast;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(partedPast == 0);
    CHECK(partedShortCircuit > 0);
}

TEST_CASE("a rank 2-3 tie can change a block", "[aquifer]") {
    // One input inside the lemma's population, run through the shipped
    // decision both ways: three water sources at levels 20 (nearest), 0
    // and 30, the second and third tied at a gap of 24 (s = 0.04), a block
    // at y 10 and a density of -0.4 — inside the overworld's reach.
    //
    //   ranked:  P12 = 2 * 9.5/1.5       -> -0.4 + 0.04 * 12.67 > 0: stone
    //   swapped: P13 = 2 * (3 - 9.5)/10  -> -0.4 + 0.04 * -1.3   < 0
    //            P12 at s^2              -> -0.4 + 0.0016 * 12.67 < 0
    //            P23 = 2 * (3 + 10.5)/3  -> -0.4 + 0.04 * 9      < 0: water
    //
    // So the clean-room spec's 0 of 792 338 (open question 9) is how rarely
    // real generation lands here, not a property of Q6.6 — and the server's
    // deep-floor probe has 27 such blocks, every one the later-wins way
    // (vanilla_aquifer_deepfloor_test.cpp).
    using stratum::aquifer::Source;
    const std::array<SourceStatus, 3> status = {
        SourceStatus{.level = 20, .type = FluidType::Default},
        SourceStatus{.level = 0, .type = FluidType::Default},
        SourceStatus{.level = 30, .type = FluidType::Default}};
    const auto sourceAt = [](std::int32_t id, std::int32_t distanceSq) {
        return Source{.cell = CellIndex{.x = id, .y = 0, .z = 0},
                      .centre = CellIndex{.x = id, .y = 0, .z = 0},
                      .distanceSq = distanceSq};
    };
    const auto statusOf = [&status](const Source& source) {
        return source.cell.x < 3 ? status.at(static_cast<std::size_t>(source.cell.x))
                                 : SourceStatus{.level = 20, .type = FluidType::Default};
    };
    const auto noBarrierNoise = [](std::int32_t, std::int32_t, std::int32_t) { return 0.0; };
    const AquiferQuery query{.x = 0, .y = 10, .z = 0, .density = -0.4, .seaLevel = kSea};
    Selection ranked{};
    ranked.ranked = {sourceAt(0, 100), sourceAt(1, 124), sourceAt(2, 124), sourceAt(3, 200)};
    Selection swapped = ranked;
    std::swap(swapped.ranked[1], swapped.ranked[2]);
    const auto decide = [&](const Selection& selection, double density) {
        AquiferQuery at = query;
        at.density = density;
        return stratum::aquifer::computeSubstanceFrom(selection, at, statusOf, noBarrierNoise);
    };
    CHECK(decide(ranked, -0.4).substance == Substance::Solid);
    const SubstanceAt asSwapped = decide(swapped, -0.4);
    CHECK(asSwapped.substance == Substance::Fluid);
    CHECK(asSwapped.fluidType == FluidType::Default);
    // Inside a window of density only: at -0.3 both write stone, and at the
    // overworld's floor, -11/24, the two still part.
    CHECK(decide(swapped, -0.3).substance == Substance::Solid);
    CHECK(decide(ranked, -11.0 / 24.0).substance == Substance::Solid);
    CHECK(decide(swapped, -11.0 / 24.0).substance == Substance::Fluid);
}

namespace {

/// What one block's decision came to, every field a block or a mark can show.
[[nodiscard]] bool sameDecision(const SubstanceAt& a, const SubstanceAt& b) {
    return a.substance == b.substance && a.fluidType == b.fluidType &&
           a.fluidUpdate == b.fluidUpdate;
}

} // namespace

TEST_CASE("the barrier value is read only where the predicate weighs it, and no block can tell",
          "[aquifer]") {
    // Three ways to decide the same blocks, over stub fields that put wet and
    // dry sources of many levels and both types side by side, with a
    // varying surface (some scans abort): the shipped path, reading the
    // `barrier` value only on demand with the candidate window memoized; the
    // same with every read made up front (BarrierReads::Always); and the
    // unmemoized `computeSubstanceWith` over `selectSources`. All three must
    // agree on every field of every block — substance, fluid type and the
    // fluid-update mark — and the on-demand path must read the value on far
    // fewer blocks, never where Q6.2 short-circuits.
    using stratum::aquifer::BarrierReads;
    using stratum::aquifer::computeSubstanceWith;
    using stratum::aquifer::NoDeepDark;
    using stratum::aquifer::Source;
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
    const auto field = [](std::uint64_t salt) {
        return [salt](std::int32_t x, std::int32_t y, std::int32_t z) {
            return stubField(salt, x, y, z);
        };
    };
    const auto floodedness = field(11);
    const auto spread = field(12);
    const auto lava = field(13);
    const auto psl = [](std::int32_t x, std::int32_t, std::int32_t z) {
        return 20.0 + (90.0 * stubField(14, x, 0, z)); // [-70, 110): some scans abort
    };
    long long readsOnDemand = 0;
    long long readsAlways = 0;
    long long readsUnmemoized = 0;
    long long readWithoutCompeting = 0;
    const auto barrierCounting = [](long long& reads) {
        return [&reads](std::int32_t x, std::int32_t y, std::int32_t z) {
            ++reads;
            return 1.3 * stubField(15, x, y, z);
        };
    };
    const auto onDemand = barrierCounting(readsOnDemand);
    const auto always = barrierCounting(readsAlways);
    const auto unmemoized = barrierCounting(readsUnmemoized);

    StatusCache onDemandCache;
    StatusCache alwaysCache;
    StatusCache statusOnly;
    long long blocks = 0;
    long long disagreements = 0;
    long long stone = 0;
    long long fluid = 0;
    long long marked = 0;
    for (std::int32_t z = -45; z < 45; z += 4) {
        for (std::int32_t x = -45; x < 45; x += 4) {
            for (std::int32_t y = -70; y < 100; ++y) {
                const AquiferQuery query{.x = x, .y = y, .z = z, .density = -0.4, .seaLevel = kSea};
                const long long before = readsOnDemand;
                const SubstanceAt shipped =
                    computeSubstance(centres, query, onDemandCache, onDemand, floodedness, spread,
                                     lava, psl, NoDeepDark{});
                const SubstanceAt eager =
                    computeSubstance(centres, query, alwaysCache, always, floodedness, spread, lava,
                                     psl, NoDeepDark{}, BarrierReads::Always);
                const SubstanceAt reference = computeSubstanceWith(
                    centres, query,
                    [&](const Source& ranked) {
                        return statusOnly.statusOf(ranked, kSea, psl, floodedness, spread, lava,
                                                   NoDeepDark{});
                    },
                    unmemoized, BarrierReads::Always);
                ++blocks;
                disagreements += static_cast<long long>(!sameDecision(shipped, eager) ||
                                                        !sameDecision(shipped, reference));
                if (readsOnDemand > before) {
                    const Selection selection = selectSources(centres, x, y, z);
                    readWithoutCompeting +=
                        static_cast<long long>(!stratum::aquifer::nearestPairCompetes(
                            selection.ranked[0].distanceSq, selection.ranked[1].distanceSq));
                }
                stone += static_cast<long long>(shipped.substance == Substance::Solid);
                fluid += static_cast<long long>(shipped.substance == Substance::Fluid);
                marked += static_cast<long long>(shipped.fluidUpdate);
            }
        }
    }
    CHECK(blocks == 23LL * 23 * 170);
    CHECK(disagreements == 0);
    CHECK(readWithoutCompeting == 0);
    CHECK(readsAlways == readsUnmemoized);
    // Not vacuous: barriers, fluid and marks all occur, the value is read
    // where it is weighed, and on far fewer blocks than it used to be.
    CHECK(stone > 0);
    CHECK(fluid > 0);
    CHECK(marked > 0);
    CHECK(readsOnDemand > 0);
    CHECK(readsOnDemand * 4 < readsAlways);
}

TEST_CASE("the candidate window is drawn once per home cell, for the world it was drawn from",
          "[aquifer]") {
    // StatusCache::candidatesOf is candidatesFor, memoized: the same twelve
    // candidates for every home cell, on both sides of the origin and in an
    // order that alternates between neighbouring homes (which share no slot)
    // and returns to them; and a different world's lattice is redrawn, not
    // served from the slots another world filled.
    using stratum::aquifer::candidatesFor;
    const CentreSource modern{42, stratum::density::RandomSource::Xoroshiro};
    const CentreSource other{43, stratum::density::RandomSource::Xoroshiro};
    const CentreSource legacy{42, stratum::density::RandomSource::Legacy};
    StatusCache cache;
    long long compared = 0;
    long long wrong = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::int32_t y = -7; y <= 7; ++y) {
            for (std::int32_t z = -3; z <= 3; ++z) {
                for (std::int32_t x = -3; x <= 3; ++x) {
                    for (const std::int32_t dx : {0, 1, 0, -1}) {
                        const CellIndex home{.x = x + dx, .y = y, .z = z};
                        for (const CentreSource* world : {&modern, &modern, &other, &legacy}) {
                            ++compared;
                            wrong += static_cast<long long>(cache.candidatesOf(*world, home) !=
                                                            candidatesFor(*world, home));
                        }
                    }
                }
            }
        }
    }
    CHECK(compared == 2LL * 15 * 7 * 7 * 4 * 4);
    CHECK(wrong == 0);
    // The three worlds really do draw different windows, so the check above
    // would have caught a slot served across them.
    const CellIndex home{.x = -2, .y = 3, .z = 1};
    CHECK(candidatesFor(modern, home) != candidatesFor(other, home));
    CHECK(candidatesFor(modern, home) != candidatesFor(legacy, home));
}
