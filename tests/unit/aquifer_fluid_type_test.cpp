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

    // The ceiling is NARROWED to {-10, -9}, not yet pinned to one value
    // (`aquifer-fluidtype-probe.sh` group B/C): -11 and -10 both measured as
    // lava, -8 measured as water — -10, this file's own default, fits every
    // reading; -9 could not be reached directly to test (fluid_type.hpp's
    // own header explains why, and what the next attempt should try).
    CHECK(fluidTypeOf(deep(0.9, -11)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.9, -10)) == FluidType::Lava);
    CHECK(fluidTypeOf(deep(0.9, -8)) == FluidType::Default);

    // Both ends of the ORIGINAL, wider bracket still gate correctly, since
    // {-10, -9} sits inside [-14, -5] — kept as a coarser sanity check.
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
