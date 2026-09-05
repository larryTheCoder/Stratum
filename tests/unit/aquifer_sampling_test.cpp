// Stratum — where the aquifer reads its router inputs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The centres below are this build's own, from `CentreSource(42)`, and every
// one of them was checked against the position the vanilla server was measured
// to read at. What the vectors pin is the SHAPE of each read — which space it
// is in, which quantity is inside the division, and which way the division
// rounds — because those are what an implementation gets wrong.
#include <stratum/aquifer/sampling.hpp>
#include <stratum/javamath.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::floodednessSample;
using stratum::aquifer::levelBand;
using stratum::aquifer::SamplePos;
using stratum::aquifer::spreadSample;

TEST_CASE("floodedness is read at the cell's own centre, verbatim", "[aquifer]") {
    const CentreSource source{42};

    // Seven cells whose centres this build produces and the server was read
    // against. The point of listing them rather than asserting an identity is
    // that each is also a rejection of the low corner and of every
    // quantisation: 71 is not 64, 53 is not 48, 7 is not 0.
    struct Case {
        CellIndex cell;
        CellIndex centre;
    };

    const Case cases[] = {
        {{4, 0, 3}, {71, 7, 53}},   {{4, 0, 4}, {70, 6, 72}},  {{1, 0, 5}, {22, 7, 85}},
        {{6, 0, 5}, {105, 4, 89}},  {{3, 1, 3}, {53, 20, 57}}, {{6, 1, 3}, {100, 19, 55}},
        {{7, 2, 3}, {119, 31, 53}},
    };
    for (const auto& one : cases) {
        INFO("cell " << one.cell.x << "," << one.cell.y << "," << one.cell.z);
        const CellIndex centre = source.centreOf(one.cell.x, one.cell.y, one.cell.z);
        CHECK(centre == one.centre);
        CHECK(floodednessSample(centre) ==
              SamplePos{.x = one.centre.x, .y = one.centre.y, .z = one.centre.z});

        // The rival an implementer is likeliest to write: the cell's low
        // corner. Every one of these separates it.
        CHECK(floodednessSample(centre) !=
              SamplePos{.x = 16 * one.cell.x, .y = 12 * one.cell.y, .z = 16 * one.cell.z});
    }

    // The nearest rival by score was the centre quantised to two, and it is
    // worth pinning why that took thousands of cells to kill rather than one.
    // It COINCIDES with the centre wherever all three coordinates happen to be
    // even — cell (4,0,4) centres at (70,6,72) and cannot tell them apart —
    // and separates only where one is odd. A test that asserted a difference
    // on every cell would be asserting something false about the measurement.
    const CellIndex even = source.centreOf(4, 0, 4);
    CHECK(even == CellIndex{70, 6, 72});
    CHECK(floodednessSample(even) ==
          SamplePos{.x = even.x & ~1, .y = even.y & ~1, .z = even.z & ~1});

    const CellIndex odd = source.centreOf(7, 2, 3);
    CHECK(odd == CellIndex{119, 31, 53});
    CHECK(floodednessSample(odd) != SamplePos{.x = odd.x & ~1, .y = odd.y & ~1, .z = odd.z & ~1});
}

TEST_CASE("the spread is read in lattice indices, not in block space", "[aquifer]") {
    const CentreSource source{42};
    // The discriminating cell. Its centre y is 200, so the band is 5 — but the
    // cell's own layer times the pitch is 192, whose band is 4. Those are
    // forty blocks apart in the ladder. This is the cell that separates
    // `floorDiv(centreY, 40)` from `floorDiv(12 * cellY, 40)`, and the second
    // is the one an implementer writes by accident.
    const CellIndex cell{2, 16, 5};
    const CellIndex centre = source.centreOf(cell.x, cell.y, cell.z);
    CHECK(centre == CellIndex{32, 200, 85});
    CHECK(levelBand(centre.y) == 5);
    CHECK(stratum::javamath::floorDiv(12 * cell.y, 40) == 4);
    CHECK(spreadSample(cell, centre) == SamplePos{.x = 2, .y = 5, .z = 5});

    // And it is not in block space at all: the x and z are cell indices, so a
    // block-coordinate reading would be sixteen times too large.
    CHECK(spreadSample(cell, centre) != SamplePos{.x = centre.x, .y = 5, .z = centre.z});
}

TEST_CASE("the spread's band divides toward negative infinity", "[aquifer]") {
    // The trap. A truncating division agrees on every non-negative centre here
    // and is one band high on every negative one, which folds two bands into
    // one at the origin and moves every ladder below it.
    CHECK(levelBand(200) == 5);
    CHECK(levelBand(192) == 4);
    CHECK(levelBand(40) == 1);
    CHECK(levelBand(0) == 0);
    CHECK(levelBand(-1) == -1); // C++ (-1 / 40) is 0
    CHECK(levelBand(-8) == -1); // C++ (-8 / 40) is 0
    CHECK(levelBand(-40) == -1);
    CHECK(levelBand(-54) == -2);

    // The measured negative-coordinate cell, where the whole chain runs
    // negative at once: the cell index, the centre and the band.
    const CentreSource source{42};
    const CellIndex cell{-7, -1, -1};
    const CellIndex centre = source.centreOf(cell.x, cell.y, cell.z);
    CHECK(centre == CellIndex{-106, -8, -9});
    CHECK(spreadSample(cell, centre) == SamplePos{.x = -7, .y = -1, .z = -1});
    CHECK(floodednessSample(centre) == SamplePos{.x = -106, .y = -8, .z = -9});
}

TEST_CASE("the ladder is built from the band the spread is addressed by", "[aquifer]") {
    // Not two derivations of the same number that happen to agree — one band,
    // used twice. If they ever disagree, one of the two is reading the wrong
    // cell.
    const CentreSource source{42};
    const CellIndex cell{2, 16, 5};
    const CellIndex centre = source.centreOf(cell.x, cell.y, cell.z);
    const std::int32_t band = spreadSample(cell, centre).y;
    CHECK(stratum::aquifer::ladderLevel(centre.y, 1000, 0.0) ==
          (stratum::aquifer::kBasePitch * band) + stratum::aquifer::kBasePhase);

    // With the spread on, the offset lands on the same ladder. -1.45 floors to
    // -15 and +1.65 to +15, both through a division that must round down.
    CHECK(stratum::aquifer::ladderLevel(centre.y, 1000, -1.45) == 205);
    CHECK(stratum::aquifer::ladderLevel(centre.y, 1000, 1.65) == 235);
    // The spread's own negative case: -0.05 scales to -0.5, floors to -1, and
    // a truncating `/ 3` would give 0 rather than -3.
    CHECK(stratum::aquifer::spreadOffset(-0.05) == -3);
}

TEST_CASE("the surface level is read at absolute zero", "[aquifer]") {
    // The one axis of psl's read that is settled. It does not move with the
    // cell, the world floor or the sea.
    CHECK(stratum::aquifer::kPreliminarySurfaceSampleY == 0);
}
