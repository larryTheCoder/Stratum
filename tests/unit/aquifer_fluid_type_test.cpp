// Stratum — whether an aquifer's fluid is water or lava.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The numbers these vectors encode came off the vanilla server through the
// `elava` probe arm — see the conformance case of the same name, and
// `fluid_type.hpp` for what was measured and what was not. What is pinned
// here is the SHAPE: which axis contracts by how much, which comparison is on
// a magnitude, and which gate comes first.
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/substance.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

using stratum::aquifer::CellIndex;
using stratum::aquifer::FluidType;
using stratum::aquifer::FluidTypeAt;
using stratum::aquifer::fluidTypeOf;
using stratum::aquifer::kLavaIndexPitchXZ;
using stratum::aquifer::kLavaIndexPitchY;
using stratum::aquifer::kLavaLevelCeiling;
using stratum::aquifer::kLavaThreshold;
using stratum::aquifer::lavaSample;
using stratum::aquifer::SamplePos;
using stratum::aquifer::spreadSample;

namespace {

/// A source deep enough for the override to be in play, at an ordinary sea.
[[nodiscard]] constexpr FluidTypeAt deep(const double lava, const std::int32_t level = -20) {
    return FluidTypeAt{.centreY = -20, .level = level, .seaLevel = 63, .lava = lava};
}

} // namespace

TEST_CASE("the lava override reads a magnitude, not a signed value", "[aquifer]") {
    // Measured: signed scores 0.96896 on the same 3125 cells that give the
    // magnitude 0.99873, so the sign genuinely does not matter.
    CHECK(fluidTypeOf(deep(0.5)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(-0.5)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.1)) == FluidType::Default);
    CHECK(fluidTypeOf(deep(-0.1)) == FluidType::Default);
}

TEST_CASE("the lava threshold sits at three tenths", "[aquifer]") {
    // A peak rather than a plateau: on the probe, 0.25 leaves 78 false
    // positives and 0.35 leaves 31 false negatives, where 0.30 leaves 2 of
    // each. It is also the only aquifer constant published anywhere this
    // project may read, which is why it was worth measuring rather than
    // adopting.
    CHECK(fluidTypeOf(deep(0.2999)) == FluidType::Default);
    CHECK(fluidTypeOf(deep(0.3001)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(-0.3001)) == FluidType::Lava);

    // STRICTNESS IS MEASURED, to the ULP (`aquifer-fluidtype-probe.sh` group
    // A): a `lava` entry driven to the exact double 0.3 came back water on
    // the server, and the very next representable double above it came back
    // lava — the same exact-double technique that pinned the barrier at 1.5,
    // this time confirming rather than assuming the strict `>` this file
    // already reads with.
    CHECK(fluidTypeOf(deep(kLavaThreshold)) == FluidType::Default);
    CHECK(fluidTypeOf(deep(std::nextafter(kLavaThreshold, 0.0))) == FluidType::Default);
    CHECK(fluidTypeOf(deep(std::nextafter(kLavaThreshold, 1.0))) == FluidType::Lava);
}

TEST_CASE("a source too high up never turns to lava", "[aquifer]") {
    CHECK(fluidTypeOf(deep(0.9, kLavaLevelCeiling)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.9, kLavaLevelCeiling + 1)) == FluidType::Default);
    CHECK(fluidTypeOf(deep(0.9, 63)) == FluidType::Default);

    // The ceiling is PINNED at -10 (`aquifer-fluidtype-probe.sh --group d`).
    // The earlier pass could only bracket it to {-10, -9} — sweeping the
    // spread steps the level in threes and skipped both candidates — so
    // these assertions were satisfied by EITHER value and did not
    // discriminate. Group D reached -9 by two routes the mod-3 ladder does
    // not constrain (the sea branch, and the psl cap at two different sea
    // levels) and read water there on three seeds, 16384 of 16384 columns
    // per dimension with 0 lava; -10 read lava the same way.
    //
    // The -9 case below is therefore the one that pins the constant, and it
    // is written as a LITERAL on purpose: every other assertion in this case
    // is phrased relative to `kLavaLevelCeiling` and so would survive the
    // constant moving to -9, which is exactly how the old bracket went
    // unnoticed. This one fails if it moves.
    CHECK(fluidTypeOf(deep(0.9, -11)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.9, -10)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.9, -9)) == FluidType::Default);
    CHECK(fluidTypeOf(deep(0.9, -8)) == FluidType::Default);

    // Both ends of the ORIGINAL, wider bracket still gate correctly, since
    // -10 sits inside [-14, -5] — kept as a coarser sanity check.
    CHECK(fluidTypeOf(deep(0.9, -14)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.9, -4)) == FluidType::Default);
}

TEST_CASE("a source below the global lava sea is lava whatever its noise says", "[aquifer]") {
    // Not from this corpus — nothing is observable below the sea — but from
    // the level rule's own measurement, and it moves with `sea_level` exactly
    // as `lambdaLevel` does.
    CHECK(fluidTypeOf(FluidTypeAt{.centreY = -55, .level = 63, .seaLevel = 63, .lava = 0.0}) ==
          FluidType::Lava);
    CHECK(fluidTypeOf(FluidTypeAt{.centreY = -54, .level = 63, .seaLevel = 63, .lava = 0.0}) ==
          FluidType::Default);

    // At a sea below the lava level the boundary is the SEA, not -54.
    CHECK(fluidTypeOf(FluidTypeAt{.centreY = -55, .level = 0, .seaLevel = -100, .lava = 0.0}) ==
          FluidType::Default);
    CHECK(fluidTypeOf(FluidTypeAt{.centreY = -101, .level = 0, .seaLevel = -100, .lava = 0.0}) ==
          FluidType::Lava);
}

TEST_CASE("a DRY source is exempt from the lava override", "[aquifer]") {
    // Q5.8's `L != never` conjunct. NOT MEASURED, and it must not be
    // described as though it were: no world this project has measured Π on
    // can even ask (fluid_type.hpp's header says exactly why — both declare
    // `lava` a constant 0.0, short-circuiting the level term, and the
    // selection worlds pin floodedness at 0.5 so no source is ever dry). It
    // is carried on the spec's word, and it became REPRESENTABLE only when
    // `cellFluidLevel` began reporting `kNeverLevel` rather than `lambda`
    // for a dry source — before that this case could not have been written.
    CHECK(fluidTypeOf(deep(0.9, stratum::aquifer::kNeverLevel)) == FluidType::Default);
    // And it is the SENTINEL that exempts it, not merely being far down: a
    // level one above the sentinel is still deep enough to turn to lava.
    CHECK(fluidTypeOf(deep(0.9, stratum::aquifer::kNeverLevel + 1)) == FluidType::Lava);

    // It is also provably INERT downstream, which is why carrying it costs
    // nothing: a source at the sentinel reads fluid at no `y` any world has,
    // and every consumer of a source's type is guarded by a reading.
    CHECK(stratum::aquifer::kNeverLevel < -30000);
}

TEST_CASE("lava is read on a sixty-four block lattice, and the spread is not", "[aquifer]") {
    // The trap this pair exists to make hard: two router entries with the
    // same SHAPE — a contracted index lattice — and different numbers.
    CHECK(kLavaIndexPitchXZ == 64);
    CHECK(kLavaIndexPitchY == 40);

    CHECK(lavaSample(CellIndex{.x = 0, .y = 0, .z = 0}) == SamplePos{.x = 0, .y = 0, .z = 0});
    CHECK(lavaSample(CellIndex{.x = 63, .y = 39, .z = 63}) == SamplePos{.x = 0, .y = 0, .z = 0});
    CHECK(lavaSample(CellIndex{.x = 64, .y = 40, .z = 64}) == SamplePos{.x = 1, .y = 1, .z = 1});

    // floorDiv, not `/`. Every one of these is a cell the origin-quadrant
    // probe harness cannot reach, and truncation is right on none of them.
    CHECK(lavaSample(CellIndex{.x = -1, .y = -1, .z = -1}) == SamplePos{.x = -1, .y = -1, .z = -1});
    CHECK(lavaSample(CellIndex{.x = -64, .y = -40, .z = -64}) ==
          SamplePos{.x = -1, .y = -1, .z = -1});
    CHECK(lavaSample(CellIndex{.x = -65, .y = -41, .z = -65}) ==
          SamplePos{.x = -2, .y = -2, .z = -2});

    // And the two entries really do part company. A centre at x = 100 sits in
    // spread index 6 and lava index 1; collapsing them would be silent.
    const CellIndex centre{.x = 100, .y = 30, .z = 100};
    const CellIndex cell{.x = 6, .y = 2, .z = 6};
    CHECK(spreadSample(cell, centre).x == 6);
    CHECK(lavaSample(centre).x == 1);
    // They agree on y, which is the same 40-block band, and that agreement is
    // measured rather than assumed: 20 and 80 both lose.
    CHECK(spreadSample(cell, centre).y == lavaSample(centre).y);
}

TEST_CASE("a near-surface sea is not lava for its centre sitting below the lava sea", "[aquifer]") {
    // A cell close under a submerged surface takes the sea from that surface
    // (spec Q5.3(b)), so its centre's height does not type it. On
    // aquifer-capfloor-probe.sh's cf58 arm (psl -58, centres between -62
    // and lambda flooding from the near surface) typing those cells lava
    // built 20 462 blocks of barrier against their water neighbours that the
    // server does not have.
    using stratum::aquifer::CellFluid;
    using stratum::aquifer::PslRead;
    using stratum::aquifer::sourceStatus;
    const PslRead surface{.gate = -58, .cap = -58, .anchor = -58, .aborted = false};
    const CellFluid nearSurface{
        .centreY = -57, .surface = surface, .seaLevel = 63, .floodedness = 0.6};
    const auto status = sourceStatus(nearSurface, 0.0);
    CHECK(status.level == 63);
    CHECK(status.type == FluidType::Default);

    // The trailing guard's sea is the other way round: a cell centred below
    // the lava sea that floods through its own floodedness is the lava sea.
    const PslRead high{.gate = 200, .cap = 200, .anchor = 200, .aborted = false};
    const CellFluid deep{.centreY = -60, .surface = high, .seaLevel = 63, .floodedness = 0.95};
    const auto guarded = sourceStatus(deep, 0.0);
    CHECK(guarded.level == stratum::aquifer::kLavaLevel);
    CHECK(guarded.type == FluidType::Lava);
}

TEST_CASE("the lava override does not reach a short-circuit sea", "[aquifer]") {
    // Both of the near-surface path's seas are the global picker's status at
    // or above lambda, typed before Q5.8 is reached (spec Q5.3), and the
    // server agrees: on aquifer-fluidnear-probe.sh's worlds, at sea levels
    // -40, -20, -12 and -10 with `lava` 0.5 or -0.5, every source such a sea
    // owns is water. The cell's own sea at the same level is lava.
    using stratum::aquifer::CellFluid;
    using stratum::aquifer::constantSurface;
    using stratum::aquifer::PslRead;
    using stratum::aquifer::sourceStatus;
    const auto typed = [](const CellFluid& cell, const double lava) {
        const auto status = sourceStatus(cell, lava);
        CHECK(status.level == -20);
        return status.type;
    };
    // The near-surface return, centred within twenty of the surface and above.
    const CellFluid within{.centreY = -30, .surface = constantSurface(-40), .seaLevel = -20};
    const CellFluid above{.centreY = -18, .surface = constantSurface(-40), .seaLevel = -20};
    CHECK(typed(within, 0.5) == FluidType::Default);
    CHECK(typed(within, -0.5) == FluidType::Default);
    CHECK(typed(above, 0.5) == FluidType::Default);
    // An aborted scan's sea, more than twenty above an aborting surface.
    const PslRead aborting{.gate = -64, .cap = -64, .anchor = -64, .aborted = true};
    const CellFluid abortedSea{.centreY = -40, .surface = aborting, .seaLevel = -20};
    CHECK(typed(abortedSea, 0.5) == FluidType::Default);
    // The cell's own sea branch (floodedness past 0.8) is not a short-circuit.
    const CellFluid ownSea{
        .centreY = -30, .surface = constantSurface(96), .seaLevel = -20, .floodedness = 0.9};
    CHECK(typed(ownSea, 0.5) == FluidType::Lava);

    // The type rule alone: the origin decides it, not the level.
    CHECK(
        fluidTypeOf(FluidTypeAt{
            .centreY = -30, .level = -20, .seaLevel = -20, .lava = 0.5, .fromNearSurface = true}) ==
        FluidType::Default);
    CHECK(fluidTypeOf(FluidTypeAt{.centreY = -30, .level = -20, .seaLevel = -20, .lava = 0.5}) ==
          FluidType::Lava);
}
