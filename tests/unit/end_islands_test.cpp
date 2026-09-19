// Stratum — the End island field, at the level of its own arithmetic.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHERE THE AUTHORITY IS. The field as a whole is settled against the vanilla
// server, not here: tests/conformance/golden_end_test.cpp generates the End
// block for block on eight golden seeds and on two probe regions r.0.0 cannot
// reach, and tools/analysis/end-islands-analyze.cpp reads the field itself out
// of a probe dimension and scores it column by column (1024 of 1024, worst
// error 0.3 blocks of island height, at the readback's own resolution).
//
// What is here is what those cannot say cheaply:
//
//   * the central term's arithmetic at points where it is EXACT — Pythagorean
//     grid coordinates, where `100 - distance * 8` is a whole number, so the
//     expected values are computed from the documented constants rather than
//     captured from this build;
//   * the two clamps, at both ends;
//   * floorDiv, not truncation, on the way in — the reading the negative
//     probe region settled, pinned at a point where the two disagree;
//   * and a REGRESSION PIN on the seeding, whose authority is the conformance
//     case and not this file: a refactor that loses the 17292-step skip has to
//     fail somewhere that runs without fixtures.
#include <stratum/noise/end_islands.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>

using Catch::Approx;
using stratum::noise::EndIslands;

namespace {

/// Bit equality, so that "these two are the same float" can be said without
/// -Wfloat-equal — and said exactly, which is what is meant here: the outer
/// term either contributed nothing or it did.
[[nodiscard]] std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

} // namespace

TEST_CASE("the central island is 100 minus eight times the grid distance, clamped",
          "[noise][end_islands]") {
    // 3-4-5, 6-8-10, 9-12-15 and 15-20-25: the distance is a whole number, so
    // every expected value below is arithmetic on the field's own constants
    // and none of it is a capture of what this build happens to return.
    CHECK(EndIslands::centralIslandHeight(3, 4) == Approx(60.0));   // 100 - 5 * 8
    CHECK(EndIslands::centralIslandHeight(6, 8) == Approx(20.0));   // 100 - 10 * 8
    CHECK(EndIslands::centralIslandHeight(9, 12) == Approx(-20.0)); // 100 - 15 * 8

    // The upper clamp: 100 at the origin, held down to 80.
    CHECK(EndIslands::centralIslandHeight(0, 0) == Approx(80.0));
    // The lower clamp: 100 - 25 * 8 is -100 exactly, and further out stays
    // there rather than running away.
    CHECK(EndIslands::centralIslandHeight(15, 20) == Approx(-100.0));
    CHECK(EndIslands::centralIslandHeight(150, 200) == Approx(-100.0));

    // Sign-symmetric, because only the squared distance is read.
    CHECK(EndIslands::centralIslandHeight(-3, 4) == Approx(60.0));
    CHECK(EndIslands::centralIslandHeight(3, -4) == Approx(60.0));
    CHECK(EndIslands::centralIslandHeight(-3, -4) == Approx(60.0));
}

TEST_CASE("near the origin the field IS the central island", "[noise][end_islands]") {
    // The outer term is gated on `cellX^2 + cellZ^2 > 4096`, with the cell
    // being the grid coordinate halved, and the neighbourhood searched
    // reaching 12 cells. At grid (15, 20) the furthest neighbour is (19, 22),
    // whose 845 is nowhere near 4096 — so no outer island can contribute, at
    // any seed, and the two must agree exactly.
    for (const std::int64_t seed : {std::int64_t{0}, std::int64_t{42}, std::int64_t{-1}}) {
        const EndIslands islands = EndIslands::fromWorldSeed(seed);
        CHECK(bits(islands.heightAt(3, 4)) == bits(EndIslands::centralIslandHeight(3, 4)));
        CHECK(bits(islands.heightAt(9, 12)) == bits(EndIslands::centralIslandHeight(9, 12)));
        CHECK(bits(islands.heightAt(-15, -20)) == bits(EndIslands::centralIslandHeight(-15, -20)));
    }
}

TEST_CASE("the density is the height shifted by eight and divided by 128", "[noise][end_islands]") {
    const EndIslands islands = EndIslands::fromWorldSeed(0);
    // Block (0, 0) is grid (0, 0), height 80: (80 - 8) / 128.
    CHECK(islands.sample(0, 0) == Approx(0.5625));
    // Block (24, 32) is grid (3, 4), height 60: (60 - 8) / 128.
    CHECK(islands.sample(24, 32) == Approx(0.40625));
}

TEST_CASE("the grid is reached by floorDiv, not by truncation", "[noise][end_islands][javamath]") {
    const EndIslands islands = EndIslands::fromWorldSeed(0);
    // -100 / 8 is -12.5. floorDiv gives -13; C++ and Java division both
    // truncate to -12, and the two grid cells carry visibly different
    // heights, so this point separates the readings. The negative probe
    // region (golden_end_test.cpp, r.-1.-1) is what settled which is right.
    CHECK(islands.heightAt(-13, -13) == Approx(-47.078217).epsilon(1e-6));
    CHECK(islands.heightAt(-12, -12) == Approx(-35.764496).epsilon(1e-6));
    CHECK(islands.sample(-100, -100) ==
          Approx((static_cast<double>(islands.heightAt(-13, -13)) - 8.0) / 128.0));

    // And the modulus under it: floorMod, so the within-cell offset of a
    // negative grid coordinate is 1 rather than -1. Visible only through the
    // outer term, so it is asserted where that term fires.
    CHECK(bits(islands.heightAt(-1, 0)) == bits(EndIslands::centralIslandHeight(-1, 0)));
}

TEST_CASE("only the low 48 bits of a world seed reach the End", "[noise][end_islands][seeding]") {
    // Not a curiosity — it is the denominator of every End measurement this
    // project makes. `java.util.Random` scrambles its seed as
    // `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, and that mask discards bit
    // 63 — and the multiplier never brings it back, so 2^16 different world
    // seeds share every End. 0 and LONG_MIN are one world and so are -1 and
    // LONG_MAX, which means the eight golden End regions are six distinct
    // worlds, and golden_end_test.cpp says so.
    //
    // Confirmed against the server independently of this arithmetic: those
    // two pairs of golden regions differ in 0 of 2097152 blocks each.
    CHECK(EndIslands::fromWorldSeed(0).sampleSimplexSeeding(2048, 0) ==
          Approx(EndIslands::fromWorldSeed(-9223372036854775807 - 1).sampleSimplexSeeding(2048, 0))
              .epsilon(0.0));
    CHECK(EndIslands::fromWorldSeed(-1).sampleSimplexSeeding(2048, 0) ==
          Approx(EndIslands::fromWorldSeed(9223372036854775807).sampleSimplexSeeding(2048, 0))
              .epsilon(0.0));
    // And a pair that does NOT collide, so the assertions above are not just
    // "any two seeds agree".
    CHECK(EndIslands::fromWorldSeed(0).sampleSimplexSeeding(2048, 0) !=
          Approx(EndIslands::fromWorldSeed(42).sampleSimplexSeeding(2048, 0)));
}

TEST_CASE("the simplex seeding is the world seed plus a 17292-step skip",
          "[noise][end_islands][seeding]") {
    // A REGRESSION PIN, not the evidence. The evidence is the probe: with
    // this skip the server's own field comes back on 1024 of 1024 columns,
    // and without it on 4 — against a base rate of about 8 for a candidate
    // that is simply wrong. See noise::EndIslands' header.
    //
    // Cell (2048, 0) is one the probe covers, and at seed 0 it is below the
    // -0.9 gate, which is why there is an island there at all.
    CHECK(EndIslands::fromWorldSeed(0).sampleSimplexSeeding(2048, 0) ==
          Approx(-0.99292494502070439).epsilon(1e-15));
    CHECK(EndIslands::fromWorldSeed(42).sampleSimplexSeeding(2048, 0) ==
          Approx(-0.088399312978533429).epsilon(1e-15));
    CHECK(EndIslands::fromWorldSeed(-1).sampleSimplexSeeding(2048, 0) ==
          Approx(0.31966384655328528).epsilon(1e-15));

    // Different seeds give different fields — the thing that would be true of
    // a seeding that silently ignored the seed, and false of this one.
    CHECK(EndIslands::fromWorldSeed(0).sampleSimplexSeeding(2048, 0) !=
          Approx(EndIslands::fromWorldSeed(1).sampleSimplexSeeding(2048, 0)));
}
