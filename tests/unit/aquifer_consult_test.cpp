// Stratum — the aquifer's per-block consult, piece by piece, held to what it
// replaced.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// computeSubstance ranks a block's window with detail::rankFour (through
// StatusCache::rankIn, which keeps each row's distance terms), finds its home
// cell with a shift for the horizontal pitch, reads each ranked status with
// StatusCache::candidateStatus, and decides with a detail::decideRanked that
// takes the four distances and asks placesBarrier only where Q6.2 lets the
// nearest pair compete. Each piece is held here to what it replaced:
//
//   * rankFour to rankByInsertion, the ranking's definition, rank for rank and
//     candidate for candidate, on windows with ties at every rank, duplicated
//     centres and distances at the far edge of int32, and through rankIn on
//     every block of a stretch of real lattice on both sides of the origin.
//   * cellOf to a verbatim copy of the floorDiv it was, and its shift to
//     floorDiv by 16 on every int32 a sweep reaches, both signs and the ends.
//   * decideRanked to a verbatim copy of the body it replaced, over random
//     statuses, distances, densities and heights in both read modes: the same
//     answer, the same refusal, the same status asks in the same order and the
//     same barrier reads.
//   * candidateStatus to statusIn: the same answer, read on the same ask.
//
// aquifer_rank_test.cpp's "the window's status memo reads what the centre
// table read" holds the whole of computeSubstance to the path through the
// public API (rankCandidates, computeSubstanceFrom, statusOf), read for read.

#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/javamath.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace aq = stratum::aquifer;
using stratum::rng::JavaRandom;

// --- the ranking ----------------------------------------------------------

struct RankTally {
    std::size_t asked = 0;
    std::size_t wrong = 0;
    std::size_t tied = 0;
    std::string first;
};

/// rankFour, from the window's own terms, against the definition.
void compareRank(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                 const std::array<aq::Candidate, aq::kCandidateCount>& window,
                 const aq::detail::RankedFour& got, RankTally& tally) {
    ++tally.asked;
    const aq::detail::RankedWindow expected = aq::detail::rankByInsertion(x, y, z, window);
    bool same = true;
    for (std::size_t r = 0; r < aq::kRankCount; ++r) {
        same = same && got.distanceSq[r] == expected.selection.ranked[r].distanceSq &&
               got.candidate[r] == expected.candidate[r];
    }
    if (!same) {
        if (tally.wrong == 0) {
            tally.first =
                "(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")";
        }
        ++tally.wrong;
    }
    for (std::size_t r = 0; r + 1 < aq::kRankCount; ++r) {
        if (expected.selection.ranked[r].distanceSq ==
            expected.selection.ranked[r + 1].distanceSq) {
            ++tally.tied;
            break;
        }
    }
}

[[nodiscard]] aq::detail::RankedFour
rankFourAt(const std::int32_t x, const std::int32_t y, const std::int32_t z,
           const std::array<aq::Candidate, aq::kCandidateCount>& window) {
    return aq::detail::rankFour(x, window, aq::detail::distanceTermsYZ(y, z, window));
}

// --- the home cell ----------------------------------------------------------

/// cellOf as lattice.hpp wrote it before the shift, copied verbatim (only the
/// name changed).
[[nodiscard]] constexpr aq::CellIndex floorDivCellOf(const std::int32_t x, const std::int32_t y,
                                                     const std::int32_t z) noexcept {
    return aq::CellIndex{.x = stratum::javamath::floorDiv(x + aq::kCellShiftX, aq::kCellPitchX),
                         .y = stratum::javamath::floorDiv(y + aq::kCellShiftY, aq::kCellPitchY),
                         .z = stratum::javamath::floorDiv(z + aq::kCellShiftZ, aq::kCellPitchZ)};
}

// --- the decision -----------------------------------------------------------

using aq::AquiferQuery;
using aq::BarrierAt;
using aq::BarrierNoise;
using aq::BarrierReads;
using aq::BarrierSource;
using aq::FluidExit;
using aq::kRankCount;
using aq::Selection;
using aq::SourceStatus;
using aq::Substance;
using aq::SubstanceAt;

/// detail::decideRanked as substance.hpp wrote it before it took the four
/// distances and asked Q6.2 first, copied verbatim (only the name changed
/// and the namespace qualifiers added).
template<typename StatusOfRank, typename BarrierSampler>
[[nodiscard]] SubstanceAt oldDecideRanked(const Selection& selection, const AquiferQuery& query,
                                          StatusOfRank&& statusOfRank, BarrierSampler&& barrier,
                                          const BarrierReads reads) {
    // All three statuses, typed unconditionally: Π needs the type of a
    // source that reads AIR at this block as much as of one that reads fluid
    // (barrier.hpp).
    std::array<SourceStatus, 3> status{};
    for (std::size_t r = 0; r < 3; ++r) {
        status[r] = statusOfRank(r);
    }

    std::array<std::int64_t, kRankCount> distanceSq{};
    for (std::size_t r = 0; r < kRankCount; ++r) {
        distanceSq[r] = selection.ranked[r].distanceSq;
    }
    const auto fourth = [&] { return statusOfRank(std::size_t{3}); };

    // Q6.3: water resting on the global lava sea is water, and no barrier
    // is even considered — the `barrier` noise is not read.
    const bool nearestReadsFluid = query.y < status[0].level;
    if (aq::waterOverLava(query.y, query.seaLevel, status[0].level, status[0].type)) {
        return SubstanceAt{.substance = Substance::Fluid,
                           .fluidType = status[0].type,
                           .fluidUpdate = aq::fluidUpdateFlag(distanceSq, status,
                                                              FluidExit::WaterOverLava, fourth)};
    }

    const auto asBarrierSource = [&](const std::size_t r) {
        return BarrierSource{.level = status[r].level,
                             .distanceSq = selection.ranked[r].distanceSq,
                             .type = status[r].type};
    };
    BarrierAt at{
        .y = query.y,
        .density = query.density,
        .nearest = asBarrierSource(0),
        .second = asBarrierSource(1),
        .third = asBarrierSource(2),
    };
    // The `barrier` value, read where `reads` says (`BarrierReads`): on
    // first use inside the predicate — which on most blocks is never — or
    // up front, as before reads were deferred.
    const auto readBarrier = [&] { return barrier(query.x, query.y, query.z); };
    bool placed = false;
    if (reads == BarrierReads::Always) {
        at.barrier = readBarrier();
        placed = aq::placesBarrier(at);
    } else {
        BarrierNoise noise(readBarrier);
        placed = aq::placesBarrier(at, noise);
    }
    if (placed) {
        return SubstanceAt{.substance = Substance::Solid};
    }

    // The barrier has fallen through: the substance is the nearest source's
    // own reading outright (selection.hpp's own conformance case).
    if (!nearestReadsFluid) {
        return SubstanceAt{.substance = Substance::Air};
    }
    return SubstanceAt{.substance = Substance::Fluid,
                       .fluidType = status[0].type,
                       .fluidUpdate = aq::fluidUpdateFlag(distanceSq, status,
                                                          FluidExit::BarrierFellThrough, fourth)};
}

/// One decision's whole trace: its answer or refusal, every status asked
/// (by rank) and every barrier read, in order.
struct Trace {
    bool refused = false;
    std::string refusal;
    SubstanceAt answer{};
    std::vector<std::size_t> asked;
    std::size_t barrierReads = 0;

    [[nodiscard]] bool operator==(const Trace& other) const {
        return refused == other.refused && refusal == other.refusal &&
               answer.substance == other.answer.substance &&
               answer.fluidType == other.answer.fluidType &&
               answer.fluidUpdate == other.answer.fluidUpdate && asked == other.asked &&
               barrierReads == other.barrierReads;
    }
};

template<typename Decide>
[[nodiscard]] Trace traced(const std::array<SourceStatus, kRankCount>& statuses,
                           const double barrierValue, Decide&& decide) {
    Trace trace;
    const auto statusOfRank = [&](const std::size_t r) {
        trace.asked.push_back(r);
        return statuses[r];
    };
    const auto barrier = [&](std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*z*/) {
        ++trace.barrierReads;
        return barrierValue;
    };
    try {
        trace.answer = decide(statusOfRank, barrier);
    } catch (const std::exception& error) {
        trace.refused = true;
        trace.refusal = error.what();
    }
    return trace;
}

} // namespace

TEST_CASE("rankFour gives the insertion's ranks on every window it is handed", "[aquifer][rank]") {
    JavaRandom random(9187);
    RankTally tally;
    for (std::size_t trial = 0; trial < 60000; ++trial) {
        // Centres on a coarse grid near the block, so distances repeat and
        // ties land at every rank; some copy a centre outright; a few sit at
        // the far edge of int32 distances (3 * 26754^2 is just under
        // INT32_MAX), where a sum formed out of range would misorder.
        constexpr std::array<std::int32_t, 4> kSpreads{2, 4, 12, 26754};
        const std::int32_t spread = kSpreads[trial % kSpreads.size()];
        std::array<aq::Candidate, aq::kCandidateCount> window{};
        for (std::size_t i = 0; i < aq::kCandidateCount; ++i) {
            const auto axis = [&] { return random.nextInt((2 * spread) + 1) - spread; };
            aq::CellIndex centre{.x = axis(), .y = axis(), .z = axis()};
            if (i > 0 && random.nextInt(4) == 0) {
                centre =
                    window[static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(i)))]
                        .centre;
            }
            window[i] = aq::Candidate{
                .cell = aq::CellIndex{.x = static_cast<std::int32_t>(i), .y = 5, .z = -9},
                .centre = centre};
        }
        compareRank(0, 0, 0, window, rankFourAt(0, 0, 0, window), tally);
    }
    // Every centre one point: twelve ties at every rank.
    std::array<aq::Candidate, aq::kCandidateCount> same{};
    for (std::size_t i = 0; i < aq::kCandidateCount; ++i) {
        same[i] = aq::Candidate{.cell = aq::CellIndex{.x = static_cast<std::int32_t>(i)},
                                .centre = aq::CellIndex{.x = 3, .y = -4, .z = 7}};
    }
    compareRank(0, 0, 0, same, rankFourAt(0, 0, 0, same), tally);
    CAPTURE(tally.first, tally.asked, tally.tied);
    CHECK(tally.wrong == 0U);
    CHECK(tally.tied * 4 > tally.asked);
}

TEST_CASE("rankIn gives the insertion's ranks over a real lattice, row terms and all",
          "[aquifer][rank]") {
    // The shipped use: StatusCache's windows, walked in the filler's order
    // (y, then z, then x) across home-cell seams on every axis, so a window's
    // row terms are kept along x, made again at each new row, and forgotten
    // when its slot is drawn for another home. Every rank is held to the
    // definition over the window rankIn was handed.
    RankTally tally;
    for (const aq::CentreSource& centres :
         {aq::CentreSource{42, stratum::density::RandomSource::Xoroshiro},
          aq::CentreSource{-4172144997902289642, stratum::density::RandomSource::Legacy}}) {
        aq::StatusCache cache;
        for (std::int32_t y = -70; y < 90; y += 3) {
            for (std::int32_t z = -24; z < 24; ++z) {
                for (std::int32_t x = -40; x < 40; ++x) {
                    const std::size_t window = cache.windowOf(centres, aq::cellOf(x, y, z));
                    const aq::detail::RankedFour got = cache.rankIn(window, x, y, z);
                    compareRank(x, y, z, cache.candidatesIn(window), got, tally);
                    // And the window is the one selectSources ranks.
                    if (cache.candidatesIn(window) !=
                        aq::candidatesFor(centres, aq::cellOf(x, y, z))) {
                        ++tally.wrong;
                    }
                }
            }
        }
    }
    CAPTURE(tally.first, tally.asked, tally.tied);
    CHECK(tally.wrong == 0U);
    CHECK(tally.tied > 0U);
}

TEST_CASE("cellOf's horizontal shift is floorDiv by the pitch on every int32",
          "[aquifer][lattice]") {
    using stratum::javamath::floorDiv;
    using stratum::javamath::shr;
    std::size_t asked = 0;
    std::size_t wrong = 0;
    const auto check = [&](const std::int32_t v) {
        ++asked;
        wrong += shr(v, aq::kCellPitchXLog2) == floorDiv(v, aq::kCellPitchX) &&
                         shr(v, aq::kCellPitchZLog2) == floorDiv(v, aq::kCellPitchZ)
                     ? 0U
                     : 1U;
    };
    // Every value near 0 and near both ends, and a stride through the rest.
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    for (std::int32_t v = -70000; v <= 70000; ++v) {
        check(v);
    }
    for (std::int32_t d = 0; d < 70000; ++d) {
        check(kMin + d);
        check(kMax - d);
    }
    for (std::int64_t v = kMin; v <= kMax; v += 65521) {
        check(static_cast<std::int32_t>(v));
    }
    CHECK(wrong == 0U);
    CHECK(asked > 300000U);

    // And cellOf itself, against the floorDiv it was, over the block range
    // a chunk reaches (density::kMaxChunkCoordinate's), both signs.
    std::size_t cellsWrong = 0;
    JavaRandom random(31);
    for (std::size_t trial = 0; trial < 200000; ++trial) {
        const std::int32_t x = random.nextInt(1 << 30) - (1 << 29);
        const std::int32_t y = random.nextInt(4096) - 2048;
        const std::int32_t z = random.nextInt(1 << 30) - (1 << 29);
        cellsWrong += aq::cellOf(x, y, z) == floorDivCellOf(x, y, z) ? 0U : 1U;
    }
    for (std::int32_t x = -100; x <= 100; ++x) {
        for (std::int32_t y = -30; y <= 30; ++y) {
            cellsWrong += aq::cellOf(x, y, -x) == floorDivCellOf(x, y, -x) ? 0U : 1U;
        }
    }
    CHECK(cellsWrong == 0U);
}

TEST_CASE("decideRanked decides, refuses and reads as it did before Q6.2 was asked first",
          "[aquifer][substance]") {
    JavaRandom random(2718);
    std::size_t asked = 0;
    std::size_t differing = 0;
    std::array<std::size_t, 3> answers{};
    std::size_t refusals = 0;
    std::size_t barrierRead = 0;
    std::size_t fourthAsked = 0;
    std::string firstDifference;
    for (std::size_t trial = 0; trial < 200000; ++trial) {
        // Heights on both sides of the lava sea's top row, at a sea that
        // gives Q6.3 a population (-70) and the shipped one (63).
        const std::int32_t seaLevel = random.nextInt(2) == 0 ? 63 : -70;
        const std::int32_t lambda = aq::lambdaLevel(seaLevel);
        const std::int32_t y = lambda - 4 + random.nextInt(40);
        // Four sorted distances whose gaps straddle Q6.2's 25 and the flow
        // similarity's bound.
        std::array<std::int32_t, kRankCount> distances{};
        distances[0] = random.nextInt(300);
        for (std::size_t r = 1; r < kRankCount; ++r) {
            distances[r] = distances[r - 1] + random.nextInt(random.nextInt(3) == 0 ? 60 : 30);
        }
        // Statuses with levels around the block, a dry one now and then,
        // and both types; one pair in a thousand far enough apart that Π's
        // level difference is no int32 (a refusal).
        std::array<SourceStatus, kRankCount> statuses{};
        for (SourceStatus& status : statuses) {
            const std::int32_t pick = random.nextInt(20);
            status.level = pick == 0 ? aq::kNeverLevel : y - 6 + random.nextInt(13);
            status.type = random.nextInt(3) == 0 ? aq::FluidType::Lava : aq::FluidType::Default;
        }
        if (random.nextInt(1000) == 0) {
            statuses[0].level = std::numeric_limits<std::int32_t>::max();
            statuses[1].level = -std::numeric_limits<std::int32_t>::max();
        }
        const double density = -1.5 + (1.7 * random.nextDouble());
        const double barrierValue = -1.0 + (5.0 * random.nextDouble());
        const BarrierReads reads =
            random.nextInt(4) == 0 ? BarrierReads::Always : BarrierReads::WhereWeighed;
        const AquiferQuery query{.x = random.nextInt(200) - 100,
                                 .y = y,
                                 .z = random.nextInt(200) - 100,
                                 .density = density,
                                 .seaLevel = seaLevel};

        Selection selection{};
        for (std::size_t r = 0; r < kRankCount; ++r) {
            selection.ranked[r] = aq::Source{
                .cell = aq::CellIndex{.x = static_cast<std::int32_t>(r)},
                .centre = aq::CellIndex{.x = 1, .y = 2, .z = static_cast<std::int32_t>(r)},
                .distanceSq = distances[r]};
        }
        const Trace now = traced(statuses, barrierValue, [&](auto& statusOfRank, auto& barrier) {
            return aq::detail::decideRanked(distances, query, statusOfRank, barrier, reads);
        });
        const Trace before = traced(statuses, barrierValue, [&](auto& statusOfRank, auto& barrier) {
            return oldDecideRanked(selection, query, statusOfRank, barrier, reads);
        });
        // And computeSubstanceFrom, which hands decideRanked a Selection's
        // distances, takes the same decision.
        const Trace from = traced(statuses, barrierValue, [&](auto& statusOfRank, auto& barrier) {
            return aq::computeSubstanceFrom(
                selection, query,
                [&](const aq::Source& ranked) {
                    return statusOfRank(static_cast<std::size_t>(ranked.cell.x));
                },
                barrier, reads);
        });
        ++asked;
        if (!(now == before) || !(from == before)) {
            if (differing == 0) {
                firstDifference = "trial " + std::to_string(trial);
            }
            ++differing;
        }
        if (before.refused) {
            ++refusals;
        } else {
            ++answers[static_cast<std::size_t>(before.answer.substance)];
        }
        barrierRead += before.barrierReads > 0 ? 1U : 0U;
        fourthAsked += before.asked.size() > 3 ? 1U : 0U;
    }
    CAPTURE(firstDifference, asked, answers[0], answers[1], answers[2], refusals, barrierRead,
            fourthAsked);
    CHECK(differing == 0U);
    // Not vacuous: every answer, refusals, barrier reads and the fourth
    // status all occur.
    CHECK(answers[0] > 0U);
    CHECK(answers[1] > 0U);
    CHECK(answers[2] > 0U);
    CHECK(refusals > 0U);
    CHECK(barrierRead > 0U);
    CHECK(fourthAsked > 0U);
}

TEST_CASE("candidateStatus answers what statusIn answers, and reads where statusIn would",
          "[aquifer][rank]") {
    constexpr std::int32_t kSea = 63;
    const aq::CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
    using Read = std::tuple<int, std::int32_t, std::int32_t, std::int32_t>;
    const auto field = [](std::vector<Read>* log, int which, double scale, double offset) {
        return [log, which, scale, offset](std::int32_t x, std::int32_t y, std::int32_t z) {
            log->emplace_back(which, x, y, z);
            const std::int32_t mixed = (x * 31) + (y * 7) + (z * 13);
            return offset + (scale * static_cast<double>(mixed % 17) / 17.0);
        };
    };
    std::vector<Read> viaStatusIn;
    std::vector<Read> viaCandidate;
    const auto deep = [](std::vector<Read>* log) {
        return [log](std::int32_t x, std::int32_t y, std::int32_t z) {
            log->emplace_back(5, x, y, z);
            return false;
        };
    };
    aq::StatusCache statusInCache;
    aq::StatusCache candidateCache;
    std::size_t asks = 0;
    std::size_t wrong = 0;
    // Two passes over three homes: first asks and repeats, windows redrawn
    // between them (slot 0's parity is shared by the first and the last).
    for (int pass = 0; pass < 2; ++pass) {
        for (const aq::CellIndex home :
             {aq::cellOf(0, 0, 0), aq::cellOf(-37, -50, 91), aq::cellOf(32, 24, 32)}) {
            const std::size_t a = statusInCache.windowOf(centres, home);
            const std::size_t b = candidateCache.windowOf(centres, home);
            for (std::size_t c = 0; c < aq::kCandidateCount; ++c) {
                const aq::Candidate& held = statusInCache.candidatesIn(a)[c];
                const aq::Source ranked{.cell = held.cell, .centre = held.centre, .distanceSq = 7};
                const SourceStatus one = statusInCache.statusIn(
                    a, c, ranked, kSea, field(&viaStatusIn, 0, 40.0, 50.0),
                    field(&viaStatusIn, 1, 1.0, -0.2), field(&viaStatusIn, 2, 1.0, -0.5),
                    field(&viaStatusIn, 3, 1.0, -0.5), deep(&viaStatusIn));
                const SourceStatus two = candidateCache.candidateStatus(
                    b, c, 7, kSea, field(&viaCandidate, 0, 40.0, 50.0),
                    field(&viaCandidate, 1, 1.0, -0.2), field(&viaCandidate, 2, 1.0, -0.5),
                    field(&viaCandidate, 3, 1.0, -0.5), deep(&viaCandidate));
                ++asks;
                wrong += one.level == two.level && one.type == two.type ? 0U : 1U;
            }
        }
    }
    CAPTURE(asks, viaStatusIn.size());
    CHECK(wrong == 0U);
    CHECK(viaCandidate == viaStatusIn);
    CHECK(!viaStatusIn.empty());
}
