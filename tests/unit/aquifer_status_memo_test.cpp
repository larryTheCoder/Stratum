// Stratum — the aquifer's per-window status memo, one candidate at a time.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// StatusCache::statusIn answers a ranked candidate's status from its home
// window's memo, and asks the centre-keyed table (statusOf) only on the
// window's first ask of that candidate. aquifer_rank_test.cpp holds the
// whole decision to the path without the memo, read for read. This holds
// the memo itself: the first ask reads the samplers and answers what
// statusOf answers, every later ask reads nothing and answers the same —
// from the window, not the centre table — and an ask whose read refuses
// keeps nothing, so the next ask reads again.

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/density/random_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

using stratum::aquifer::CentreSource;
using stratum::aquifer::SourceStatus;
using stratum::aquifer::StatusCache;

TEST_CASE("a window asks its samplers once per candidate, and a refused ask keeps nothing",
          "[aquifer][rank]") {
    constexpr std::int32_t kSea = 63;
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};

    std::size_t reads = 0;
    bool refuse = false;
    // A field that varies with the position, so candidates differ.
    const auto field = [&reads, &refuse](double scale, double offset) {
        return [&reads, &refuse, scale, offset](std::int32_t x, std::int32_t y, std::int32_t z) {
            ++reads;
            if (refuse) {
                throw std::runtime_error("refused");
            }
            const std::int32_t mixed = (x * 31) + (y * 7) + (z * 13);
            return offset + (scale * static_cast<double>(mixed % 17) / 17.0);
        };
    };
    const auto psl = field(40.0, 50.0);
    const auto flood = field(1.0, -0.2);
    const auto spread = field(1.0, -0.5);
    const auto lava = field(1.0, -0.5);
    const auto deepDark = [&reads](std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*z*/) {
        ++reads;
        return false;
    };

    for (const stratum::aquifer::CellIndex home :
         {stratum::aquifer::cellOf(0, 0, 0), stratum::aquifer::cellOf(-37, -50, 91)}) {
        StatusCache cache;
        const std::size_t window = cache.windowOf(centres, home);
        const auto& candidates = cache.candidatesIn(window);
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            CAPTURE(c);
            const stratum::aquifer::Source ranked{
                .cell = candidates[c].cell, .centre = candidates[c].centre, .distanceSq = 0};

            // A refused first ask throws and keeps nothing.
            refuse = true;
            CHECK_THROWS_AS(
                cache.statusIn(window, c, ranked, kSea, psl, flood, spread, lava, deepDark),
                std::runtime_error);
            refuse = false;

            // The next ask reads, and answers what the centre table does.
            reads = 0;
            const SourceStatus first =
                cache.statusIn(window, c, ranked, kSea, psl, flood, spread, lava, deepDark);
            CHECK(reads > 0U);
            StatusCache reference;
            const SourceStatus expected =
                reference.statusOf(ranked, kSea, psl, flood, spread, lava, deepDark);
            CHECK(first.level == expected.level);
            CHECK(first.type == expected.type);

            // Every later ask reads nothing, and answers the same.
            reads = 0;
            const SourceStatus again =
                cache.statusIn(window, c, ranked, kSea, psl, flood, spread, lava, deepDark);
            CHECK(reads == 0U);
            CHECK(again.level == first.level);
            CHECK(again.type == first.type);

            // From the window's own memo, not the centre table: asked of
            // the window and the candidate alone, it answers the
            // candidate's status reading nothing even when handed a Source
            // whose centre no ask has named — which a caller never does
            // (the source must be the candidate's), so only a probe: a
            // window that kept nothing would take that centre to the table,
            // which has never seen it, and read.
            reads = 0;
            const stratum::aquifer::Source unseen{
                .cell = ranked.cell,
                .centre = stratum::aquifer::CellIndex{.x = ranked.centre.x + 100003,
                                                      .y = ranked.centre.y,
                                                      .z = ranked.centre.z - 100019},
                .distanceSq = 0};
            const SourceStatus held =
                cache.statusIn(window, c, unseen, kSea, psl, flood, spread, lava, deepDark);
            CHECK(reads == 0U);
            CHECK(held.level == first.level);
            CHECK(held.type == first.type);
        }
    }
}
