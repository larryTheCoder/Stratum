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

#include <array>
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
    const CentreSource centres{42};
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
    // without a server: every scan aborts, so a centre at or above lambda
    // takes the sea (it sits more than twenty blocks above the scan's
    // minimum) and every centre below it reads lambda. Which blocks are
    // pockets — the nearest source dry — is then the same at every such
    // surface, and only y_skip moves between the probe's dimensions.
    const std::int32_t lambda = lambdaLevel(kSea);
    for (const double surface : {-200.0, -93.0, -92.5, -92.0, -85.0, -81.0, -80.0, -75.0}) {
        INFO("surface " << surface);
        int wrong = 0;
        for (std::int32_t centreY = -130; centreY <= 60; ++centreY) {
            const stratum::aquifer::SourceStatus status = statusOver(surface, centreY);
            const bool expected = centreY >= lambda
                                      ? (status.level == kSea && status.type == FluidType::Default)
                                      : status.level == lambda;
            wrong += expected ? 0 : 1;
        }
        CHECK(wrong == 0);
    }
    // And -75 is the bound: one block higher and the centre at lambda itself
    // is no longer twenty blocks clear of the scan's minimum, and reads lambda.
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
