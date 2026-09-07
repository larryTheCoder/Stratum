// Stratum — which aquifer sources compete for a block.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Three of the four parts of this layer were confirmed against the vanilla
// server before any of it was written — the metric, the four ranks and the
// tie-break direction (SPEC §11, milestone MA). The fourth, the window, was
// not, and the last case here is what that looks like from inside the build.
//
// The stub candidates below carry no world seed on purpose. The ranking rule
// is a pure function of twelve distances, and giving it distances directly is
// what makes a tie a KNOWN answer rather than something a jitter draw happens
// to produce.
#include <stratum/aquifer/selection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using stratum::aquifer::Candidate;
using stratum::aquifer::CellIndex;
using stratum::aquifer::cellOf;
using stratum::aquifer::CentreSource;
using stratum::aquifer::kCandidateCount;
using stratum::aquifer::kCandidateWindow;
using stratum::aquifer::kNoSource;
using stratum::aquifer::kRankCount;
using stratum::aquifer::rankCandidates;
using stratum::aquifer::Selection;
using stratum::aquifer::selectSources;
using stratum::aquifer::squaredDistanceTo;

namespace {

/// A candidate whose centre sits at exactly `distance` squared from the
/// origin, along x. Its cell index is used only to tell the candidates apart,
/// so it doubles as a label.
[[nodiscard]] Candidate at(const std::int32_t label, const std::int32_t offsetX) noexcept {
    return Candidate{.cell = CellIndex{.x = label, .y = 0, .z = 0},
                     .centre = CellIndex{.x = offsetX, .y = 0, .z = 0}};
}

/// The labels the ranking put in each of the four ranks.
[[nodiscard]] std::array<std::int32_t, kRankCount> labels(const Selection& selection) noexcept {
    std::array<std::int32_t, kRankCount> out{};
    for (std::size_t i = 0; i < kRankCount; ++i) {
        out[i] = selection.ranked[i].cell.x;
    }
    return out;
}

} // namespace

TEST_CASE("the candidate window is twelve cells, forward in x and z", "[aquifer]") {
    REQUIRE(kCandidateWindow.size() == kCandidateCount);

    // Forward only horizontally, symmetric vertically. The asymmetry is the
    // whole reason the shift of Q3.2 exists, and the reason the window can
    // miss a nearer cell (see the last case in this file).
    int forwardX = 0;
    int forwardZ = 0;
    int lowY = 0;
    int highY = 0;
    for (const auto& offset : kCandidateWindow) {
        CHECK(offset.dx >= 0);
        CHECK(offset.dx <= 1);
        CHECK(offset.dz >= 0);
        CHECK(offset.dz <= 1);
        CHECK(offset.dy >= -1);
        CHECK(offset.dy <= 1);
        forwardX += static_cast<int>(offset.dx == 1);
        forwardZ += static_cast<int>(offset.dz == 1);
        lowY += static_cast<int>(offset.dy == -1);
        highY += static_cast<int>(offset.dy == 1);
    }
    CHECK(forwardX == 6);
    CHECK(forwardZ == 6);
    CHECK(lowY == 4);
    CHECK(highY == 4);

    // Every offset distinct, and the home cell among them.
    int home = 0;
    for (std::size_t i = 0; i < kCandidateCount; ++i) {
        home += static_cast<int>(kCandidateWindow[i].dx == 0 && kCandidateWindow[i].dy == 0 &&
                                 kCandidateWindow[i].dz == 0);
        for (std::size_t j = i + 1; j < kCandidateCount; ++j) {
            CHECK_FALSE(kCandidateWindow[i] == kCandidateWindow[j]);
        }
    }
    CHECK(home == 1);
}

TEST_CASE("the window's iteration order is x, then y, then z", "[aquifer]") {
    // Load-bearing rather than cosmetic: ties displace toward the LATER
    // candidate, so the order decides which of two equidistant cells holds a
    // rank. x outermost, y in the middle, z innermost.
    std::size_t i = 0;
    for (std::int32_t dx = 0; dx <= 1; ++dx) {
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dz = 0; dz <= 1; ++dz) {
                INFO("index " << i);
                CHECK(kCandidateWindow[i].dx == dx);
                CHECK(kCandidateWindow[i].dy == dy);
                CHECK(kCandidateWindow[i].dz == dz);
                ++i;
            }
        }
    }
    CHECK(i == kCandidateCount);
}

TEST_CASE("the metric is integer squared euclidean from the block itself", "[aquifer]") {
    const CellIndex centre{.x = 10, .y = 20, .z = 30};
    CHECK(squaredDistanceTo(centre, 13, 24, 42) == 169); // 9 + 16 + 144
    CHECK(squaredDistanceTo(centre, 10, 20, 30) == 0);
    // Signs cancel: the two blocks either side of a centre are equidistant,
    // which is exactly the configuration a half-block offset would break.
    CHECK(squaredDistanceTo(centre, 9, 20, 30) == squaredDistanceTo(centre, 11, 20, 30));
}

TEST_CASE("the block's own position is the metric's origin, not its centre", "[aquifer]") {
    // A discriminating vector rather than a restatement. Two centres one block
    // either side of the block: on integer coordinates they TIE, so the
    // tie-break decides and the later candidate wins. Measuring from the block
    // CENTRE at x + 0.5 makes the right-hand centre strictly nearer — 0.25
    // against 2.25 — so there is no tie and the earlier candidate would win.
    //
    // Q4.2, confirmed against the server at 1742/1742 and 1690/1690 on blocks
    // chosen because the two models disagree; the `+0.5` variant scored 0.
    const std::array<Candidate, 2> pair{at(1, -1), at(2, 1)};
    const Selection selection = rankCandidates(0, 0, 0, pair);

    CHECK(selection.ranked[0].distanceSq == 1);
    CHECK(selection.ranked[1].distanceSq == 1);
    CHECK(selection.nearest().cell.x == 2);
    CHECK(selection.separation() == 0);
}

TEST_CASE("a tie displaces toward the later candidate at rank one", "[aquifer]") {
    // Q4.4. This build's own earlier reading — first candidate holds — scores
    // 12.3% against the server where this one scores 228/228 and 334/334.
    const std::array<Candidate, 3> three{at(1, 5), at(2, 5), at(3, 5)};
    const Selection selection = rankCandidates(0, 0, 0, three);

    // All three equidistant, so each in turn displaces the one before it and
    // the whole column cascades: last in is nearest, first in is rank three.
    CHECK(labels(selection) == std::array<std::int32_t, kRankCount>{3, 2, 1, 0});
    CHECK(selection.ranked[0].distanceSq == 25);
    CHECK(selection.ranked[1].distanceSq == 25);
    CHECK(selection.ranked[2].distanceSq == 25);
    CHECK(selection.ranked[3].distanceSq == kNoSource);
}

TEST_CASE("the tie rule holds independently at every one of the four ranks", "[aquifer]") {
    // Four sitting tenants at four distinct distances, then one challenger per
    // rank. Each challenger ties the tenant it targets and must take that
    // rank, pushing the rest down by one — including out of rank four.
    const std::array<Candidate, 4> seated{at(1, 1), at(2, 2), at(3, 3), at(4, 4)};

    SECTION("rank two") {
        std::vector<Candidate> all(seated.begin(), seated.end());
        all.push_back(at(9, 2));
        const Selection selection = rankCandidates(0, 0, 0, all);
        CHECK(labels(selection) == std::array<std::int32_t, kRankCount>{1, 9, 2, 3});
    }
    SECTION("rank three") {
        std::vector<Candidate> all(seated.begin(), seated.end());
        all.push_back(at(9, 3));
        const Selection selection = rankCandidates(0, 0, 0, all);
        CHECK(labels(selection) == std::array<std::int32_t, kRankCount>{1, 2, 9, 3});
    }
    SECTION("rank four") {
        std::vector<Candidate> all(seated.begin(), seated.end());
        all.push_back(at(9, 4));
        const Selection selection = rankCandidates(0, 0, 0, all);
        CHECK(labels(selection) == std::array<std::int32_t, kRankCount>{1, 2, 3, 9});
    }
    SECTION("past rank four the candidate is dropped") {
        // Strictly farther than the sitting fourth, so nothing displaces. The
        // control for the three sections above: without it they would pass for
        // a rule that admits everything.
        std::vector<Candidate> all(seated.begin(), seated.end());
        all.push_back(at(9, 5));
        const Selection selection = rankCandidates(0, 0, 0, all);
        CHECK(labels(selection) == std::array<std::int32_t, kRankCount>{1, 2, 3, 4});
    }
}

TEST_CASE("selection over a real seed is ordered and reaches its own cell", "[aquifer]") {
    const CentreSource centres{42};

    std::size_t blocks = 0;
    for (std::int32_t x = -40; x < 40; x += 3) {
        for (std::int32_t z = -40; z < 40; z += 3) {
            for (std::int32_t y = -60; y < 60; y += 7) {
                const Selection selection = selectSources(centres, x, y, z);
                const CellIndex home = cellOf(x, y, z);

                bool sawHome = false;
                for (std::size_t i = 0; i < kRankCount; ++i) {
                    // Twelve candidates always fill all four ranks.
                    REQUIRE(selection.ranked[i].distanceSq != kNoSource);
                    if (i > 0) {
                        REQUIRE(selection.ranked[i - 1].distanceSq <=
                                selection.ranked[i].distanceSq);
                    }
                    // The centre carried alongside each rank is the one the
                    // distance was taken from.
                    REQUIRE(selection.ranked[i].distanceSq ==
                            squaredDistanceTo(selection.ranked[i].centre, x, y, z));
                    sawHome = sawHome || selection.ranked[i].cell == home;
                }
                // The home cell is in the window by construction, but it is
                // not always in the top four — it only has to be a candidate.
                CHECK(selection.separation() >= 0);
                (void)sawHome;
                ++blocks;
            }
        }
    }
    REQUIRE(blocks > 5000);
}

TEST_CASE("every candidate lies within the window's own distance bound", "[aquifer]") {
    // The header claims the squared distance cannot exceed 1284, which is what
    // lets it be an int32 rather than the int64 a general distance needs.
    // Checked over enough blocks to cover every residue of the shift.
    const CentreSource centres{-1};
    std::int32_t worst = 0;
    for (std::int32_t x = 0; x < 32; ++x) {
        for (std::int32_t z = 0; z < 32; ++z) {
            for (std::int32_t y = -12; y < 12; ++y) {
                const Selection selection = selectSources(centres, x, y, z);
                for (const auto& rank : selection.ranked) {
                    worst = worst > rank.distanceSq ? worst : rank.distanceSq;
                }
            }
        }
    }
    INFO("worst squared distance " << worst);
    CHECK(worst <= 1284);
}

TEST_CASE("the shift is what makes a forward-only window bracket the block", "[aquifer]") {
    // The spec's own justification for the shift of Q3.2: a window that runs
    // forward only in x and z would otherwise sit entirely to one side of its
    // own block. Measured here as "is the block between the lowest and highest
    // candidate centre on each axis", which needs no brute-force search and no
    // world beyond the twelve centres already drawn.
    //
    // It is NOT exactly always, and that is a refinement of the spec rather
    // than agreement with it: over 786432 blocks on five seeds the horizontal
    // rate runs 0.9978 to 1.0000, never below. The vertical rate is exactly
    // 1.0000 by construction, the y offsets being symmetric.
    //
    // Without the shift the same measurement gives 0.855 to 0.888, which is
    // what gives this case its teeth.
    for (const std::int64_t seed : {std::int64_t{42}, std::int64_t{-1}}) {
        const CentreSource centres{seed};
        long long blocks = 0;
        long long bracketedXZ = 0;
        long long bracketedY = 0;
        for (std::int32_t x = -32; x < 32; ++x) {
            for (std::int32_t z = -32; z < 32; ++z) {
                for (std::int32_t y = -12; y < 12; ++y) {
                    const auto candidates =
                        stratum::aquifer::candidatesFor(centres, cellOf(x, y, z));
                    std::int32_t lowX = candidates[0].centre.x;
                    std::int32_t highX = candidates[0].centre.x;
                    std::int32_t lowY = candidates[0].centre.y;
                    std::int32_t highY = candidates[0].centre.y;
                    std::int32_t lowZ = candidates[0].centre.z;
                    std::int32_t highZ = candidates[0].centre.z;
                    for (const auto& candidate : candidates) {
                        lowX = lowX < candidate.centre.x ? lowX : candidate.centre.x;
                        highX = highX > candidate.centre.x ? highX : candidate.centre.x;
                        lowY = lowY < candidate.centre.y ? lowY : candidate.centre.y;
                        highY = highY > candidate.centre.y ? highY : candidate.centre.y;
                        lowZ = lowZ < candidate.centre.z ? lowZ : candidate.centre.z;
                        highZ = highZ > candidate.centre.z ? highZ : candidate.centre.z;
                    }
                    bracketedXZ +=
                        static_cast<int>(lowX <= x && x <= highX && lowZ <= z && z <= highZ);
                    bracketedY += static_cast<int>(lowY <= y && y <= highY);
                    ++blocks;
                }
            }
        }
        INFO("seed " << seed << ": " << bracketedXZ << " of " << blocks << " bracketed in x and z");
        CHECK(bracketedY == blocks);
        CHECK(bracketedXZ * 100 > blocks * 99);
    }
}

TEST_CASE("the symmetric window is why Q4.1 is the one part still untested", "[aquifer]") {
    // The spec's window is asymmetric; the obvious rival is the symmetric
    // 27-cell set. This case is not an assertion that ours is right — nothing
    // in this project has shown that — it is the SHAPE OF THE BLOCKER, kept
    // executable so that it speaks up if it ever stops being true.
    //
    // The two sets agree on the NEAREST source essentially always (about two
    // blocks in a million disagree), and where they part at rank 2 the nearest
    // pair has already stopped competing, so the spec's Q6.2 short-circuit
    // makes the difference invisible in any block the server writes. That is
    // exactly why 6291456-block barrier corpora scored the two identically.
    //
    // If a rank-2 disagreement ever lands INSIDE the barrier-reachable shell
    // often enough to measure, Q4.1 becomes testable and this case is the
    // place that says so.
    const CentreSource centres{42};
    long long blocks = 0;
    long long differAtRankOne = 0;
    long long differAtRankTwo = 0;
    long long differInShell = 0;

    for (std::int32_t x = -32; x < 32; ++x) {
        for (std::int32_t z = -32; z < 32; ++z) {
            for (std::int32_t y = -12; y < 12; ++y) {
                const CellIndex home = cellOf(x, y, z);
                const Selection ours = selectSources(centres, x, y, z);

                std::vector<Candidate> symmetric;
                symmetric.reserve(27);
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    for (std::int32_t dy = -1; dy <= 1; ++dy) {
                        for (std::int32_t dz = -1; dz <= 1; ++dz) {
                            const CellIndex cell{
                                .x = home.x + dx, .y = home.y + dy, .z = home.z + dz};
                            symmetric.push_back(Candidate{
                                .cell = cell, .centre = centres.centreOf(cell.x, cell.y, cell.z)});
                        }
                    }
                }
                const Selection rival = rankCandidates(x, y, z, symmetric);

                differAtRankOne += static_cast<int>(!(rival.ranked[0].cell == ours.ranked[0].cell));
                const bool rankTwo = !(rival.ranked[1].cell == ours.ranked[1].cell);
                differAtRankTwo += static_cast<int>(rankTwo);
                // 25 is the spec's Q6.1 divisor, and it is the one constant
                // here this project has NOT re-measured. `barrier.hpp` owns it.
                differInShell += static_cast<int>(rankTwo && ours.separation() < 25);
                ++blocks;
            }
        }
    }

    INFO("blocks " << blocks << ", rank 1 differs " << differAtRankOne << ", rank 2 differs "
                   << differAtRankTwo << ", of those in the barrier shell " << differInShell);
    REQUIRE(blocks > 90000);

    // The marker the header points at. It is not decoration: while it stands,
    // the window below is the spec's hypothesis rather than this project's
    // finding, and anything that leans on it inherits that.
    STATIC_REQUIRE(stratum::aquifer::kWindowIsUntested);

    // The two sets DO differ, or there would be nothing to be untested about.
    CHECK(differAtRankTwo > 0);
    // And essentially never where a barrier could form. Measured at 44 of 3993
    // over 3538944 blocks on this seed, so a handful is expected here rather
    // than a clean zero — the claim is a rate, not an invariant.
    CHECK(differInShell * 20 < differAtRankTwo);
}
