// Stratum — the aquifer's ranking by keys, and its status memo, held to what
// they replaced.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `rankCandidates` used to run its definition as written: each candidate
// takes the first rank whose distance is at least its own and pushes the
// rest down, `<=` throughout (Q4.4: the later of a tie displaces). It now
// folds 64-bit keys — distance, then the index's complement — into a sorted
// four and reads the ranks back. `insertionRank` below is a VERBATIM copy of
// the code it replaced, and every case compares the two Source for Source,
// rank for rank, on spans of every length from empty to past the window,
// with ties at every rank, distances up to the edge of int32 and duplicated
// centres. `detail::rankWindow`'s candidate indices are held to the cells
// they name.
//
// `computeSubstance` now reads each ranked status from its candidate
// window's own memo (StatusCache::statusIn) and asks the centre-keyed table
// only on a window's first use of a candidate. The old path is still
// reachable through the public API — rankCandidates over candidatesOf, then
// computeSubstanceFrom with StatusCache::statusOf per ranked source — and
// the case below runs both over the same blocks with recording samplers:
// the same decisions, and the same sampler reads at the same positions in
// the same order, so no status is computed at another moment.

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {

using stratum::aquifer::AquiferQuery;
using stratum::aquifer::Candidate;
using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::kCandidateCount;
using stratum::aquifer::kRankCount;
using stratum::aquifer::rankCandidates;
using stratum::aquifer::Selection;
using stratum::aquifer::Source;
using stratum::aquifer::squaredDistanceTo;
using stratum::aquifer::StatusCache;
using stratum::aquifer::SubstanceAt;
using stratum::rng::JavaRandom;

/// The ranking as lib/src/aquifer_selection.cpp ran it before the keys,
/// copied verbatim (only the name changed).
[[nodiscard]] Selection insertionRank(const std::int32_t x, const std::int32_t y,
                                      const std::int32_t z,
                                      const std::span<const Candidate> candidates) noexcept {
    Selection out{};
    for (const Candidate& candidate : candidates) {
        const Source source{.cell = candidate.cell,
                            .centre = candidate.centre,
                            .distanceSq = squaredDistanceTo(candidate.centre, x, y, z)};

        // Every comparison is `<=`, never `<`. On equality the incoming
        // candidate takes the rank and the sitting occupant cascades down —
        // which is the whole of Q4.4, and the reason the window's iteration
        // order is part of the algorithm rather than a detail of this loop.
        if (source.distanceSq <= out.ranked[0].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = out.ranked[1];
            out.ranked[1] = out.ranked[0];
            out.ranked[0] = source;
        } else if (source.distanceSq <= out.ranked[1].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = out.ranked[1];
            out.ranked[1] = source;
        } else if (source.distanceSq <= out.ranked[2].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = source;
        } else if (source.distanceSq <= out.ranked[3].distanceSq) {
            out.ranked[3] = source;
        }
    }
    return out;
}

/// A candidate whose cell carries its position in the span, so a wrong rank
/// is visible even where two candidates share a centre.
[[nodiscard]] Candidate candidateAt(std::size_t label, const CellIndex centre) {
    return Candidate{.cell = CellIndex{.x = static_cast<std::int32_t>(label), .y = -7, .z = 3},
                     .centre = centre};
}

struct Tally {
    std::size_t asked = 0;
    std::size_t wrong = 0;
    std::size_t wrongIndex = 0;
    /// Spans where two of the ranked sources sit at one distance.
    std::size_t tied = 0;
    std::string first;
};

void compare(const std::int32_t x, const std::int32_t y, const std::int32_t z,
             const std::vector<Candidate>& candidates, Tally& tally) {
    ++tally.asked;
    const Selection expected = insertionRank(x, y, z, candidates);
    const Selection keyed = rankCandidates(x, y, z, candidates);
    const stratum::aquifer::detail::RankedWindow window =
        stratum::aquifer::detail::rankWindow(x, y, z, candidates);
    if (keyed.ranked != expected.ranked || window.selection.ranked != expected.ranked) {
        if (tally.wrong == 0) {
            tally.first = "span of " + std::to_string(candidates.size()) + " at (" +
                          std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) +
                          ")";
        }
        ++tally.wrong;
    }
    for (std::size_t r = 0; r < std::min(candidates.size(), kRankCount); ++r) {
        const std::size_t index = window.candidate[r];
        tally.wrongIndex += index < candidates.size() &&
                                    candidates[index].cell == window.selection.ranked[r].cell &&
                                    candidates[index].centre == window.selection.ranked[r].centre
                                ? 0U
                                : 1U;
    }
    for (std::size_t r = 0; r + 1 < kRankCount; ++r) {
        if (r + 1 < candidates.size() &&
            expected.ranked[r].distanceSq == expected.ranked[r + 1].distanceSq) {
            ++tally.tied;
            break;
        }
    }
}

} // namespace

TEST_CASE("the keyed ranking gives the insertion's ranks on every span", "[aquifer][rank]") {
    JavaRandom random(4321);
    Tally tally;
    for (std::size_t length = 0; length <= 20; ++length) {
        for (std::size_t trial = 0; trial < 3000; ++trial) {
            // Centres on a coarse grid near the block, so that distances
            // repeat and ties land at every rank; some spans copy a centre
            // outright.
            constexpr std::array<std::int32_t, 3> kSpreads{2, 4, 12};
            const std::int32_t spreadOf = kSpreads[trial % kSpreads.size()];
            std::vector<Candidate> candidates;
            for (std::size_t i = 0; i < length; ++i) {
                CellIndex centre{.x = random.nextInt((2 * spreadOf) + 1) - spreadOf,
                                 .y = random.nextInt((2 * spreadOf) + 1) - spreadOf,
                                 .z = random.nextInt((2 * spreadOf) + 1) - spreadOf};
                if (i > 0 && random.nextInt(5) == 0) {
                    centre = candidates[static_cast<std::size_t>(
                                            random.nextInt(static_cast<std::int32_t>(i)))]
                                 .centre;
                }
                candidates.push_back(candidateAt(i, centre));
            }
            compare(0, 0, 0, candidates, tally);
        }
    }
    CAPTURE(tally.first, tally.asked, tally.tied);
    CHECK(tally.wrong == 0U);
    CHECK(tally.wrongIndex == 0U);
    CHECK(tally.tied * 4 > tally.asked);
}

TEST_CASE("the keyed ranking holds at the far edge of int32 distances", "[aquifer][rank]") {
    // Distances up to 3 * 26754^2 = 2147329548, a hair under INT32_MAX,
    // where a key that lost the distance's top bits or sign would misorder.
    // (INT32_MAX itself is no sum of three squares, since it is 7 mod 8.)
    JavaRandom random(77);
    Tally tally;
    constexpr std::int32_t kFar = 26754;
    for (std::size_t length = 1; length <= 14; ++length) {
        for (std::size_t trial = 0; trial < 2000; ++trial) {
            std::vector<Candidate> candidates;
            for (std::size_t i = 0; i < length; ++i) {
                const auto axis = [&]() {
                    const std::int32_t pick = random.nextInt(4);
                    if (pick == 0) {
                        return kFar;
                    }
                    if (pick == 1) {
                        return -kFar;
                    }
                    return random.nextInt((2 * kFar) + 1) - kFar;
                };
                candidates.push_back(
                    candidateAt(i, CellIndex{.x = axis(), .y = axis(), .z = axis()}));
            }
            compare(0, 0, 0, candidates, tally);
        }
    }
    // Every centre at the far corner: all tied at the largest distance.
    std::vector<Candidate> corner;
    for (std::size_t i = 0; i < 9; ++i) {
        corner.push_back(candidateAt(i, CellIndex{.x = kFar, .y = -kFar, .z = kFar}));
    }
    compare(0, 0, 0, corner, tally);
    CAPTURE(tally.first, tally.asked);
    CHECK(tally.wrong == 0U);
    CHECK(tally.wrongIndex == 0U);
}

TEST_CASE("the keyed ranking gives the insertion's ranks over a real lattice", "[aquifer][rank]") {
    // The shipped use: twelve candidates from candidatesFor, every block of a
    // stretch of the world on both sides of the origin, both sources.
    Tally tally;
    for (const CentreSource& centres :
         {CentreSource{42, stratum::density::RandomSource::Xoroshiro},
          CentreSource{-4172144997902289642, stratum::density::RandomSource::Legacy}}) {
        for (std::int32_t y = -70; y < 90; y += 3) {
            for (std::int32_t z = -40; z < 40; z += 3) {
                for (std::int32_t x = -40; x < 40; ++x) {
                    const std::array<Candidate, kCandidateCount> window =
                        stratum::aquifer::candidatesFor(centres, stratum::aquifer::cellOf(x, y, z));
                    compare(x, y, z, std::vector<Candidate>(window.begin(), window.end()), tally);
                }
            }
        }
    }
    CAPTURE(tally.first, tally.asked, tally.tied);
    CHECK(tally.wrong == 0U);
    CHECK(tally.wrongIndex == 0U);
    CHECK(tally.tied > 0U);
}

namespace {

/// splitmix64's finaliser over a position: a stub field in [-1, 1), as
/// aquifer_substance_test.cpp's.
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
    return (static_cast<double>(h >> 11U) / 4503599627370496.0) - 1.0;
}

/// Every sampler read, in order: which sampler, and where.
using Read = std::tuple<int, std::int32_t, std::int32_t, std::int32_t>;

} // namespace

TEST_CASE("the window's status memo reads what the centre table read, when it read it",
          "[aquifer][rank]") {
    using stratum::aquifer::computeSubstance;
    using stratum::aquifer::computeSubstanceFrom;
    constexpr std::int32_t kSea = 63;
    for (const CentreSource& centres :
         {CentreSource{42, stratum::density::RandomSource::Xoroshiro},
          CentreSource{9001, stratum::density::RandomSource::Legacy}}) {
        std::vector<Read> shippedReads;
        std::vector<Read> referenceReads;
        const auto recording = [](std::vector<Read>* log, int which, std::uint64_t salt,
                                  double scale, double offset) {
            return
                [log, which, salt, scale, offset](std::int32_t x, std::int32_t y, std::int32_t z) {
                    log->emplace_back(which, x, y, z);
                    return offset + (scale * stubField(salt, x, y, z));
                };
        };
        const auto deepRecording = [](std::vector<Read>* log) {
            return [log](std::int32_t x, std::int32_t y, std::int32_t z) {
                log->emplace_back(5, x, y, z);
                return stubField(16, x, y, z) > 0.6;
            };
        };
        // One set of samplers per path, each logging into its own list.
        const auto barrierS = recording(&shippedReads, 0, 15, 1.3, 0.0);
        const auto floodS = recording(&shippedReads, 1, 11, 1.0, 0.0);
        const auto spreadS = recording(&shippedReads, 2, 12, 1.0, 0.0);
        const auto lavaS = recording(&shippedReads, 3, 13, 1.0, 0.0);
        const auto pslS = recording(&shippedReads, 4, 14, 90.0, 20.0);
        const auto deepS = deepRecording(&shippedReads);
        const auto barrierR = recording(&referenceReads, 0, 15, 1.3, 0.0);
        const auto floodR = recording(&referenceReads, 1, 11, 1.0, 0.0);
        const auto spreadR = recording(&referenceReads, 2, 12, 1.0, 0.0);
        const auto lavaR = recording(&referenceReads, 3, 13, 1.0, 0.0);
        const auto pslR = recording(&referenceReads, 4, 14, 90.0, 20.0);
        const auto deepR = deepRecording(&referenceReads);

        StatusCache shippedCache;
        StatusCache referenceCache;
        std::size_t blocks = 0;
        std::size_t differing = 0;
        std::size_t fluid = 0;
        std::size_t stone = 0;
        // The filler's order within a cell row: y, then z, then x innermost,
        // across home-cell seams on every axis so windows are redrawn and
        // slots reused.
        for (std::int32_t y = -60; y < 80; ++y) {
            for (std::int32_t z = -20; z < 20; z += 3) {
                for (std::int32_t x = -24; x < 24; ++x) {
                    const AquiferQuery query{
                        .x = x, .y = y, .z = z, .density = -0.3, .seaLevel = kSea};
                    const SubstanceAt shipped = computeSubstance(
                        centres, query, shippedCache, barrierS, floodS, spreadS, lavaS, pslS, deepS,
                        stratum::aquifer::BarrierReads::WhereWeighed);
                    // The path computeSubstance took before the memo.
                    SubstanceAt reference{};
                    if (stratum::aquifer::globalReadsLava(query.y, query.seaLevel)) {
                        reference = computeSubstance(centres, query, referenceCache, barrierR,
                                                     floodR, spreadR, lavaR, pslR, deepR);
                    } else {
                        const Selection selection =
                            rankCandidates(x, y, z,
                                           referenceCache.candidatesOf(
                                               centres, stratum::aquifer::cellOf(x, y, z)));
                        reference = computeSubstanceFrom(
                            selection, query,
                            [&](const Source& ranked) {
                                return referenceCache.statusOf(ranked, kSea, pslR, floodR, spreadR,
                                                               lavaR, deepR);
                            },
                            barrierR);
                    }
                    ++blocks;
                    differing += shipped.substance == reference.substance &&
                                         shipped.fluidType == reference.fluidType &&
                                         shipped.fluidUpdate == reference.fluidUpdate
                                     ? 0U
                                     : 1U;
                    fluid += shipped.substance == stratum::aquifer::Substance::Fluid ? 1U : 0U;
                    stone += shipped.substance == stratum::aquifer::Substance::Solid ? 1U : 0U;
                }
            }
        }
        CAPTURE(blocks, fluid, stone, shippedReads.size());
        CHECK(differing == 0U);
        CHECK(shippedReads == referenceReads);
        // Not vacuous: both outcomes occur, and statuses were computed.
        CHECK(fluid > 0U);
        CHECK(stone > 0U);
        CHECK(shippedReads.size() > 1000U);
    }
}
