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
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------------
// The horizontal read of `preliminary_surface_level`.
//
// These run against a stub oracle rather than a noise field, so they need no
// fixture and no density implementation: each one states what the server was
// measured to do and fails the implementations that were measured NOT to be
// it. Between them they pin the window's membership, its asymmetry, the scan
// order, the abort's strictness, the rounding, the anchor's division, and the
// fact that one scan produces two different values.
// ---------------------------------------------------------------------------

namespace {
using stratum::aquifer::kPreliminarySurfaceSampleY;
using stratum::aquifer::PslRead;
using stratum::aquifer::readPreliminarySurface;

/// A psl oracle: a default everywhere, overridden at named offsets from the
/// anchor. Offsets rather than absolute positions, so a case reads as the
/// experiment it came from.
class Oracle {
public:
    Oracle(std::int32_t anchorX, std::int32_t anchorZ, double background)
        : anchorX_(anchorX), anchorZ_(anchorZ), background_(background) {}

    Oracle& at(std::int32_t dx, std::int32_t dz, double value) {
        points_.push_back({dx, dz, value});
        return *this;
    }

    double operator()(std::int32_t x, std::int32_t y, std::int32_t z) const {
        CHECK(y == kPreliminarySurfaceSampleY);
        for (const auto& one : points_) {
            if (x == anchorX_ + one.dx && z == anchorZ_ + one.dz) {
                return one.value;
            }
        }
        return background_;
    }

private:
    struct Point {
        std::int32_t dx;
        std::int32_t dz;
        double value;
    };

    std::int32_t anchorX_;
    std::int32_t anchorZ_;
    double background_;
    std::vector<Point> points_;
};

constexpr CellIndex kCentre{37, 100, 22}; // anchor (36, 20)
constexpr std::int32_t kAnchorX = 36;
constexpr std::int32_t kAnchorZ = 20;
} // namespace

TEST_CASE("the surface window's anchor floors toward negative infinity", "[aquifer]") {
    // Invisible in the origin quadrant, which is the only place the probe
    // harness ever looked — it hardcodes a forceload of 0,0..127,127, and that
    // is how a truncating reading survived 1370 dimensions. Each case below is
    // an anchor a truncating division gets wrong.
    const auto anchorOf = [](std::int32_t coordinate) {
        return stratum::javamath::floorDiv(coordinate, stratum::aquifer::kPslAnchorQuantum) *
               stratum::aquifer::kPslAnchorQuantum;
    };
    CHECK(anchorOf(37) == 36);     // positive control: truncation agrees
    CHECK(anchorOf(-3) == -4);     // truncation gives 0
    CHECK(anchorOf(-1) == -4);     // truncation gives 0
    CHECK(anchorOf(-125) == -128); // truncation gives -124
    CHECK(anchorOf(-121) == -124); // truncation gives -120
    CHECK(anchorOf(-128) == -128); // on the grid, both agree
}

TEST_CASE("only the thirteen measured offsets are in the window", "[aquifer]") {
    // V1. The window returns the high value everywhere it looks and the low
    // value everywhere it does not, so any implementation reading outside it
    // comes back with -1000.
    Oracle oracle{kAnchorX, kAnchorZ, -1000.0};
    oracle.at(0, 0, 100.0);
    for (const auto& offset : stratum::aquifer::kPslWindow) {
        oracle.at(offset.dx, offset.dz, 100.0);
    }
    CHECK(readPreliminarySurface(oracle, kCentre) == PslRead{.gate = 100, .cap = 100});

    // And the near misses, each of which a symmetric or dense window would
    // read: the spur's neighbours on the other two rows, its mirror, and the
    // positions a ±32 extent in z would add.
    for (const auto& outside : {std::pair{-48, -16}, std::pair{-48, 16}, std::pair{32, 0},
                                std::pair{0, -32}, std::pair{0, 32}}) {
        INFO("offset " << outside.first << "," << outside.second);
        Oracle probe{kAnchorX, kAnchorZ, 100.0};
        probe.at(outside.first, outside.second, -1000.0);
        CHECK(readPreliminarySurface(probe, kCentre) == PslRead{.gate = 100, .cap = 100});
    }
}

TEST_CASE("the surface read is a minimum, and its abort is strict", "[aquifer]") {
    // V2. Everything at the threshold exactly: `value < -62.0` is false there,
    // so nothing aborts and the minimum is the threshold itself.
    Oracle flat{kAnchorX, kAnchorZ, -62.0};
    CHECK(readPreliminarySurface(flat, kCentre) == PslRead{.gate = -62, .cap = -62});

    // V3. A hair below it aborts, and the two consumers part: the gate keeps
    // the anchor's own value, the cap takes the aborting sample.
    Oracle fires{kAnchorX, kAnchorZ, 50.0};
    fires.at(0, 0, 100.0).at(-32, -16, -62.01);
    CHECK(readPreliminarySurface(fires, kCentre) == PslRead{.gate = 100, .cap = -63});

    // The same case a hundredth higher does not abort — and the threshold
    // sample is then FOLDED IN rather than skipped, so both consumers come
    // back with it. An implementation that ignores samples at or below the
    // threshold instead of aborting on them returns 50 here; that reading was
    // measured at 0.866 against this one's 1.0000.
    Oracle holds{kAnchorX, kAnchorZ, 50.0};
    holds.at(0, 0, 100.0).at(-32, -16, -62.0);
    CHECK(readPreliminarySurface(holds, kCentre) == PslRead{.gate = -62, .cap = -62});
}

TEST_CASE("the anchor is read before anything can abort", "[aquifer]") {
    // V4, and the 200-of-200 observation behind it: a cell whose own anchor
    // sample is below the threshold takes THAT value, not its neighbours'
    // minimum. An implementation that scans in raster order without seeding
    // from the anchor, or that skips low samples, returns -50 here.
    Oracle oracle{kAnchorX, kAnchorZ, -50.0};
    oracle.at(0, 0, -70.0);
    CHECK(readPreliminarySurface(oracle, kCentre) == PslRead{.gate = -70, .cap = -70});
}

TEST_CASE("the surface window is scanned z-outer, ascending", "[aquifer]") {
    // V5. Two low samples. Raster order reaches (-32,-16) first and stops, so
    // (-16,0) is never seen. An x-outer or z-descending scan reaches (-16,0)
    // first and reports -40 to the gate.
    Oracle oracle{kAnchorX, kAnchorZ, 200.0};
    oracle.at(-32, -16, -64.0).at(-16, 0, -40.0);
    CHECK(readPreliminarySurface(oracle, kCentre) == PslRead{.gate = 200, .cap = -64});

    // V6. The spur is first in its own row, so a value there is folded in
    // before a later sample in the same row aborts. Placing it last — or
    // dropping it — leaves the gate at 200.
    Oracle spur{kAnchorX, kAnchorZ, 200.0};
    spur.at(-48, 0, -40.0).at(-32, 0, -64.0);
    CHECK(readPreliminarySurface(spur, kCentre) == PslRead{.gate = -40, .cap = -64});

    // V7. The spur is not mirrored: (+32, 0) is outside the window.
    Oracle mirrored{kAnchorX, kAnchorZ, 200.0};
    mirrored.at(32, 0, -40.0);
    CHECK(readPreliminarySurface(mirrored, kCentre) == PslRead{.gate = 200, .cap = 200});
}

TEST_CASE("one scan feeds the gate and the cap different values", "[aquifer]") {
    // V8. The case that proves there are two consumers rather than one. With
    // sea_level 40, floodedness 0.6, spread 0 and a centre at -30 the ladder is
    // -20, and the settled level rule yields 40 or -20 for EVERY single psl —
    // both of which leave the cell wet. It is observed dry at the lava level,
    // 858 times out of 858.
    Oracle oracle{kAnchorX, kAnchorZ, 200.0};
    oracle.at(0, -16, -70.0);
    const PslRead read = readPreliminarySurface(oracle, kCentre);
    CHECK(read == PslRead{.gate = 200, .cap = -70});

    stratum::aquifer::CellFluid cell;
    cell.centreY = -30;
    cell.preliminarySurface = read.gate;
    cell.seaLevel = 40;
    cell.floodedness = 0.6;
    // The gate keeps this cell off the ocean branch, and the cap then sinks
    // the ladder below the lava sea, where it clamps.
    CHECK(stratum::aquifer::ladderLevel(cell.centreY, read.cap, cell.spread) ==
          stratum::aquifer::kLavaLevel);
    // A single-valued implementation would put the ladder at -20 and flood it.
    CHECK(stratum::aquifer::ladderLevel(cell.centreY, read.gate, cell.spread) == -20);

    // V9. Nothing aborts, so the two coincide and the cell is on the ocean
    // branch after all.
    Oracle quiet{kAnchorX, kAnchorZ, 200.0};
    quiet.at(0, -16, -50.0);
    const PslRead same = readPreliminarySurface(quiet, kCentre);
    CHECK(same == PslRead{.gate = -50, .cap = -50});
    cell.preliminarySurface = same.gate;
    CHECK(stratum::aquifer::cellFluidLevel(cell) == 40);
}

TEST_CASE("the surface value floors toward negative infinity", "[aquifer]") {
    // V10. Truncation toward zero agrees on the positive case and is wrong on
    // the negative one, which is the whole trap.
    Oracle negative{kAnchorX, kAnchorZ, 100.0};
    negative.at(-16, 0, -0.5);
    CHECK(readPreliminarySurface(negative, kCentre) == PslRead{.gate = -1, .cap = -1});

    Oracle positive{kAnchorX, kAnchorZ, 100.0};
    positive.at(-16, 0, 40.9);
    CHECK(readPreliminarySurface(positive, kCentre) == PslRead{.gate = 40, .cap = 40});

    Oracle aborting{kAnchorX, kAnchorZ, 200.0};
    aborting.at(0, 0, 10.0).at(-16, 0, -62.5);
    CHECK(readPreliminarySurface(aborting, kCentre) == PslRead{.gate = 10, .cap = -63});
}

TEST_CASE("the surface window works off the origin quadrant", "[aquifer]") {
    // V11. The offsets are applied to the floored anchor and are not mirrored
    // when the coordinates go negative. Centre (-125, -121) anchors at
    // (-128, -124), so the aborting sample sits at (-160, -140).
    constexpr CellIndex centre{-125, 50, -121};
    Oracle oracle{-128, -124, 200.0};
    oracle.at(-32, -16, -70.0);
    CHECK(readPreliminarySurface(oracle, centre) == PslRead{.gate = 200, .cap = -70});
}

TEST_CASE("the surface is read at absolute zero, whatever the cell", "[aquifer]") {
    // V13. The oracle is the identity in y and asserts the y it is given, so a
    // read at the cell's own centre y would both fail the CHECK inside it and
    // return that y rather than zero.
    const auto identity = [](std::int32_t, std::int32_t y, std::int32_t) {
        return static_cast<double>(y);
    };
    for (const CellIndex centre : {CellIndex{37, -16, 22}, CellIndex{37, 130, 22}}) {
        INFO("centre y " << centre.y);
        CHECK(readPreliminarySurface(identity, centre) == PslRead{.gate = 0, .cap = 0});
    }
}
