// Stratum — the aquifer's tie-break, counted where the goldens can see it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Spec Q4.4: two candidate cells at the same squared distance take ranks in
// iteration order, the LATER one ahead. This build ranks that way at all
// four ranks (`rankCandidates`), and the rule had probe-block confirmation
// (228/228, 334/334) but no count on real generation of how often it
// decides a block. The clean-room spec budgets golden coverage from its own
// counts (Q4.7, "Coverage budget": rank 1-2 ties change the substance 47
// times in 4096 chunks, so ~87 chunks per expected event) and leaves one of
// them unexplained: rank 2-3 ties changed 0 blocks of 792 338 (open
// question 9).
//
// WHAT THIS RUNS. support/aquifer_tiebreak.hpp replays the first pass
// through the shipped decision (`aquifer::computeSubstanceFrom`) at every
// position where the lattice is consulted, checked block for block against
// `ChunkFiller` itself, and at every tie swaps the tied pair and decides
// again — the spec's own Q4.7 experiment. Over the whole of the eight golden
// overworld regions (8192 chunks, 72 889 063 consultations;
// tools/analysis/aquifer-tiebreak-analyze.cpp):
//
//     pair  ties        substance changed   flag changed (as narrowed)
//     1-2     797 131            65                  65
//     2-3   1 075 059             0               2 915
//     3-4   1 283 696         1 552               1 112
//
// with duplicate distances somewhere in 15.51% of selections and the whole
// strict earlier-wins ranking differing on 6.09% (the spec: 15.40%, 6.05%).
// Rank 1-2 changes the substance at 0.0079 per chunk (spec 0.0115), so the
// budget is the spec's order of magnitude: ~126 chunks per expected event
// against its 87. (The flag counts are this build's flag, set only on a
// fluid result; the spec counted the flag on every block, so they do not
// compare.)
//
// WHERE THE GOLDENS DECIDE IT. A swap that changes the substance changes a
// block the server wrote, so the golden settles the order there — unless
// fluid that moved after generation could have made the golden block from
// either answer (`tiebreak::GoldenVerdict`). Of the 65 rank 1-2 blocks, 33
// only the later-wins order can leave (water where the swap writes stone,
// a lava source where it writes air, flowing water where it writes a
// source, ...), 32 either order can (air beside two water sources, which
// the infinite-water rule fills), and NONE only the earlier-wins order can.
// Rank 3-4 is decided on all 1552 of its blocks, the same way, and the
// whole strict rival on 1595 of 1627, 32 ambiguous. This case reads the 21
// chunks holding the 33 rank 1-2 blocks the golden decides (36 rank 1-2
// blocks in all), so CI re-asks the server about every one of them each
// time it regenerates the regions.
//
// OPEN QUESTION 9, EXPLAINED. Swapping ranks 2 and 3 of a tie (d2 = d3)
// leaves s13 = s12 = s and s23 = 1, so Q6.6 weighs {s*P12, s^2*P13, s*P23}
// against {s*P13, s^2*P12, s*P23}: the same three statuses, only the
// weights moved. The two agree whenever A2 = A3, whenever the nearest
// shares a status with either (one pressure is 0 and, since D <= 0 and
// 0 < s <= 1, `D + s^2*P > 0` implies `D + s*P > 0`), and whenever d1 = d2;
// the fluid-update flag past Q6.2 is symmetric too, so a rank 2-3 flag
// change and a rank 2-3 block change come from disjoint populations — the
// flag only where Q6.2 hands the block to the nearest source, the block
// only where the barrier is weighed (tests/unit/aquifer_substance_test.cpp
// pins all three statements, and that a block CAN change). On the goldens
// the funnel is:
//
//     1 075 059 rank 2-3 ties
//       801 011 decided by Q6.2 (s12 <= 0), 1 by Q6.3
//       274 047 weighed: 7 404 with d1 = d2, 248 593 with A2 = A3,
//                        16 170 with A1 sharing a status, 1 880 all distinct
//         1 880: the two orders' predicates part on no density (1 059),
//                only below the overworld's floor of -11/24 (666) or only
//                above 0 (140), and within reach on 15 — all at
//                d2 - d1 of 22 to 24 (s <= 0.12), each window's upper end
//                at -0.066 or below, while those blocks' own densities are
//                -0.0003 to -0.085: shallower than their window every time.
//
// So the zero is rarity, not structure — and the server shows a rank 2-3
// tie deciding blocks where the population is not rare: on the deep-floor
// probe (floodedness 0.6, so neighbouring cells hold three different ladder
// levels, at densities -0.3 and -0.05), 27 blocks on 4x4 chunks of three
// seeds, every one written the later-wins way
// (vanilla_aquifer_deepfloor_test.cpp). On the goldens, weighed against
// the all-distinct blocks' own density distribution, the 15 windows predict
// about 0.35 changed blocks in 8192 chunks (an estimate, which assumes a
// window and the density it meets are independent); the spec's 4096 chunks
// would expect a fraction of one. Do not encode the zero (open question 9's
// warning stands).
//
// What the model alone decides is pinned exactly; what the golden says is
// bounded and its shape required, because CI regenerates the regions and
// the flow remnant is run-dependent (SPEC §7). Nothing here is committed:
// the regions are Mojang-derived (SPEC §12).
#include "support/aquifer_tiebreak.hpp"
#include "support/fluid_flow.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace tiebreak = stratum::test::tiebreak;

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

struct SeedSample {
    std::int64_t seed = 0;
    /// Chunks of r.0.0, as (x, z).
    std::vector<std::pair<std::int32_t, std::int32_t>> chunks;
};

/// Every chunk of the eight golden overworld regions holding a block a rank
/// 1-2 swap changes and the golden decides, as the full sweep found them.
[[nodiscard]] std::vector<SeedSample> sample() {
    return {
        {0, {{2, 26}}},
        {1, {{1, 28}, {12, 28}, {30, 5}}},
        {-1, {{8, 28}}},
        {42, {{8, 8}, {20, 5}}},
        {2891948927356891LL, {{1, 14}, {10, 2}, {13, 30}, {31, 31}}},
        {-4172144997902289642LL, {{0, 20}, {4, 13}, {5, 13}, {22, 30}, {23, 30}}},
        {9223372036854775807LL, {{1, 20}, {23, 4}, {26, 7}, {26, 8}}},
        {-9223372036854775807LL - 1, {{13, 4}}},
    };
}

} // namespace

TEST_CASE("the aquifer tie-break on the golden overworld: every tie counted and every block it "
          "decides the server's",
          "[conformance][aquifer]") {
    const std::filesystem::path worldgen = versionDir() / "worldgen";
    if (!std::filesystem::is_directory(worldgen / "noise_settings")) {
        SKIP("no worldgen fixtures under " << versionDir() << "; run tools/fetch-vanilla");
    }
    const auto overworld = stratum::data::ResourceLocation::parse("minecraft:overworld");
    const stratum::data::Pack pack = stratum::data::Pack::open(worldgen);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& settings = loaded.settings.at(overworld);
    REQUIRE(settings.aquifersEnabled);

    tiebreak::Tally total;
    std::size_t chunks = 0;
    bool regionsPresent = true;
    for (const SeedSample& seed : sample()) {
        CAPTURE(seed.seed);
        const std::filesystem::path region = versionDir() / "regions" /
                                             ("seed-" + std::to_string(seed.seed)) / "overworld" /
                                             "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            regionsPresent = false;
            break;
        }
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed.seed,
            stratum::density::RandomSource::Xoroshiro);
        const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings);
        const tiebreak::Replay replay(loaded.graph, noises, settings);
        stratum::test::GoldenRegion golden(region);
        tiebreak::Tally tally;
        for (const auto& [cx, cz] : seed.chunks) {
            REQUIRE(golden.hasChunk(cx, cz));
            stratum::terrain::ChunkBuffer filled(settings.geometry);
            filler.fill(cx, cz, filled);
            replay.chunk(cx, cz, filled, golden, tally);
            ++chunks;
        }
        // The replay is the filler, block for block and mark for mark.
        CHECK(tally.mirrorCategoryMismatch == 0U);
        CHECK(tally.mirrorFlagMismatch == 0U);
        total.absorb(tally);
    }
    if (!regionsPresent) {
        SKIP("no golden overworld regions under " << versionDir() / "regions"
                                                  << "; generate them with tools/probe-worlds");
    }
    REQUIRE(chunks == 21U);
    REQUIRE(total.blocks == 21U * 16U * 16U * 384U);

    const tiebreak::PairTally& rank12 = total.pairs[0];
    const tiebreak::PairTally& rank23 = total.pairs[1];
    const tiebreak::PairTally& rank34 = total.pairs[2];
    const tiebreak::Funnel23& funnel = total.funnel;
    INFO("consulted " << total.consulted << ", any duplicate " << total.anyDuplicate << ", min D "
                      << total.minDensity);
    INFO("ties " << rank12.ties << " / " << rank23.ties << " / " << rank34.ties << "; substance "
                 << rank12.substanceChanged << " / " << rank23.substanceChanged << " / "
                 << rank34.substanceChanged << "; flag " << rank12.flagChanged << " / "
                 << rank23.flagChanged << " / " << rank34.flagChanged << "; barrier "
                 << rank12.barrierChanged << " / " << rank23.barrierChanged << " / "
                 << rank34.barrierChanged);
    INFO("strict: differs " << total.strictRankingDiffers << ", substance "
                            << total.strictSubstanceChanged << ", flag "
                            << total.strictFlagChanged);
    INFO("funnel: short-circuit " << funnel.shortCircuit << ", water-over-lava "
                                  << funnel.waterOverLava << ", weighed " << funnel.barrierWeighed
                                  << " (three-way " << funnel.threeWay << ", A2=A3 "
                                  << funnel.secondEqualsThird << ", A1 shared "
                                  << funnel.nearestShared << ", all distinct " << funnel.allDistinct
                                  << ": empty " << funnel.windowEmpty << ", unreachable "
                                  << funnel.windowUnreachable << ", reachable "
                                  << funnel.windowReachable << ", hit " << funnel.densityInWindow
                                  << ")");
    INFO("golden 1-2: shipped " << rank12.golden.shipped << ", either " << rank12.golden.ambiguous
                                << ", swapped " << rank12.golden.swapped << ", neither "
                                << rank12.golden.neither);
    INFO("golden 3-4: shipped " << rank34.golden.shipped << ", either " << rank34.golden.ambiguous
                                << ", swapped " << rank34.golden.swapped << ", neither "
                                << rank34.golden.neither);
    INFO("golden strict: shipped " << total.strictGolden.shipped << ", either "
                                   << total.strictGolden.ambiguous << ", swapped "
                                   << total.strictGolden.swapped << ", neither "
                                   << total.strictGolden.neither);

    // The density the lattice is consulted at never leaves [-11/24, 0]: the
    // reach the rank 2-3 funnel below measures windows against.
    CHECK(total.minDensity >= tiebreak::kLowestOverworldDensity);

    // The lemma, on real generation: a rank 1-2 swap never moves the
    // barrier, and a rank 2-3 swap moves the flag only where Q6.2 decides
    // and the barrier only where all three statuses differ.
    CHECK(rank12.barrierChanged == 0U);
    CHECK(funnel.flagChangedPastShortCircuit == 0U);
    CHECK(funnel.substanceChangedOffAllDistinct == 0U);
    CHECK(funnel.barrierChangedOffAllDistinct == 0U);
    CHECK(funnel.shortCircuit + funnel.waterOverLava + funnel.barrierWeighed == rank23.ties);
    CHECK(funnel.threeWay + funnel.secondEqualsThird + funnel.nearestShared + funnel.allDistinct ==
          funnel.barrierWeighed);
    CHECK(funnel.windowEmpty + funnel.windowUnreachable + funnel.windowReachable ==
          funnel.allDistinct);

    // The model alone, pinned exactly: none of it reads the golden, so flow
    // cannot move it, and a change to any of it is a change to the selection,
    // a status or the barrier, to be explained. The full sweep's own figures
    // are in this file's header.
    CHECK(total.consulted == 225577U);
    CHECK(total.anyDuplicate == 35951U);
    CHECK(rank12.ties == 2581U);
    CHECK(rank23.ties == 3489U);
    CHECK(rank34.ties == 4010U);
    CHECK(rank12.substanceChanged == 36U);
    CHECK(rank23.substanceChanged == 0U);
    CHECK(rank34.substanceChanged == 11U);
    CHECK(rank12.flagChanged == 36U);
    CHECK(rank23.flagChanged == 8U);
    CHECK(rank34.flagChanged == 2U);
    CHECK(rank23.barrierChanged == 0U);
    CHECK(rank34.barrierChanged == 11U);
    CHECK(total.strictRankingDiffers == 13941U);
    CHECK(total.strictSubstanceChanged == 48U);
    CHECK(total.strictFlagChanged == 46U);
    CHECK(funnel.shortCircuit == 2593U);
    CHECK(funnel.waterOverLava == 0U);
    CHECK(funnel.threeWay == 34U);
    CHECK(funnel.secondEqualsThird == 723U);
    CHECK(funnel.nearestShared == 125U);
    CHECK(funnel.allDistinct == 14U);
    CHECK(funnel.windowEmpty == 8U);
    CHECK(funnel.windowUnreachable == 6U);
    CHECK(funnel.densityInWindow == 0U);

    // What the golden says: nothing only the earlier-wins order explains,
    // nothing neither explains, at any pair or under the whole strict rival.
    // How many are decided rather than ambiguous moves with the flow
    // remnant, so it is bounded: 33 on the region set first measured, 26 of
    // them a source or a stone flow cannot touch.
    for (const tiebreak::PairTally* pair : {&rank12, &rank23, &rank34}) {
        CHECK(pair->golden.swapped == 0U);
        CHECK(pair->golden.neither == 0U);
        CHECK(pair->golden.shipped + pair->golden.ambiguous == pair->substanceChanged);
    }
    CHECK(total.strictGolden.swapped == 0U);
    CHECK(total.strictGolden.neither == 0U);
    CHECK(total.strictGolden.shipped + total.strictGolden.ambiguous ==
          total.strictSubstanceChanged);
    CHECK(rank12.golden.shipped >= 26U);
}
