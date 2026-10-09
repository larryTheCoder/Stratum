// Stratum — the aquifer's tie-break, counted over the golden overworld regions.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   aquifer-tiebreak-analyze <fixtures>/1.21.11 [--stride N] [--seed S ...]
//                            [--events]
//
// Replays every N-th chunk on each axis (default 1: all 1024) of each golden
// overworld region r.0.0 (default: all eight seeds) through
// tests/support/aquifer_tiebreak.hpp — the shipped first pass, mirrored and
// checked against `ChunkFiller` block for block — and prints, per seed and
// in total, the tie events at each rank pair, what swapping the tied pair
// changes (substance, fluid-update flag, barrier verdict), what the golden
// holds where the substance changes, the whole strict earlier-wins rival,
// and the rank 2-3 funnel behind open question 9. `--events` also prints
// every substance-changing swap and every rank 2-3 tie whose three statuses
// all differ, one line each.
//
// The figures it printed, all eight seeds at stride 1 (about 35 minutes in
// a RelWithDebInfo build, two processes of four seeds each), are in SPEC §11
// ("The aquifer tie-break's reach on the goldens"); golden_aquifer_tiebreak_
// test.cpp pins the same counts on the 21 chunks where the golden decides a
// rank 1-2 tie.
#include "support/aquifer_tiebreak.hpp"
#include "support/fluid_flow.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace tiebreak = stratum::test::tiebreak;
using stratum::test::Category;

constexpr std::array<std::int64_t, 8> kSeeds = {0,
                                                1,
                                                -1,
                                                42,
                                                2891948927356891LL,
                                                -4172144997902289642LL,
                                                9223372036854775807LL,
                                                -9223372036854775807LL - 1};

[[nodiscard]] const char* nameOf(const Category c) {
    switch (c) {
        case Category::Air:
            return "air";
        case Category::Water:
            return "water";
        case Category::Lava:
            return "lava";
        case Category::Solid:
            return "solid";
    }
    return "?";
}

[[nodiscard]] const char* pairName(const std::size_t p) {
    constexpr std::array<const char*, tiebreak::kPairCount> kNames = {"1-2", "2-3", "3-4"};
    return p < kNames.size() ? kNames.at(p) : "?";
}

void printVerdict(const char* label, const tiebreak::GoldenVerdict& v) {
    std::printf("    %s golden: only the shipped order %llu, either %llu, only the swapped order "
                "%llu, neither %llu\n",
                label, static_cast<unsigned long long>(v.shipped),
                static_cast<unsigned long long>(v.ambiguous),
                static_cast<unsigned long long>(v.swapped),
                static_cast<unsigned long long>(v.neither));
}

void print(const char* label, const tiebreak::Tally& t) {
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("%s: %llu chunks, %llu blocks, %llu consulted, any duplicate %llu, min D %.9f\n",
                label, u(t.chunks), u(t.blocks), u(t.consulted), u(t.anyDuplicate), t.minDensity);
    std::printf("  mirror: category mismatches %llu, flag mismatches %llu\n",
                u(t.mirrorCategoryMismatch), u(t.mirrorFlagMismatch));
    for (std::size_t p = 0; p < tiebreak::kPairCount; ++p) {
        const tiebreak::PairTally& pair = t.pairs[p];
        std::printf("  rank %s: ties %llu, substance changed %llu, flag changed %llu, barrier "
                    "changed %llu\n",
                    pairName(p), u(pair.ties), u(pair.substanceChanged), u(pair.flagChanged),
                    u(pair.barrierChanged));
        printVerdict("", pair.golden);
    }
    std::printf("  strict earlier-wins: ranking differs %llu, substance changed %llu, flag "
                "changed %llu\n",
                u(t.strictRankingDiffers), u(t.strictSubstanceChanged), u(t.strictFlagChanged));
    printVerdict("strict", t.strictGolden);
    const tiebreak::Funnel23& f = t.funnel;
    std::printf("  rank 2-3 funnel: short-circuit %llu, water-over-lava %llu, barrier weighed %llu "
                "(three-way %llu, A2=A3 %llu, A1 shared %llu, all distinct %llu: window empty "
                "%llu, unreachable %llu, reachable %llu, D in window %llu)\n",
                u(f.shortCircuit), u(f.waterOverLava), u(f.barrierWeighed), u(f.threeWay),
                u(f.secondEqualsThird), u(f.nearestShared), u(f.allDistinct), u(f.windowEmpty),
                u(f.windowUnreachable), u(f.windowReachable), u(f.densityInWindow));
    std::printf("  rank 2-3 lemma: flag changed past short-circuit %llu, substance changed off "
                "all-distinct %llu, barrier changed off all-distinct %llu\n",
                u(f.flagChangedPastShortCircuit), u(f.substanceChangedOffAllDistinct),
                u(f.barrierChangedOffAllDistinct));
}

int run(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: aquifer-tiebreak-analyze <fixtures>/1.21.11 [--stride N] "
                             "[--seed S ...] [--events]\n");
        return 2;
    }
    const std::filesystem::path version = argv[1];
    std::int32_t stride = 1;
    bool events = false;
    std::vector<std::int64_t> seeds;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--stride" && i + 1 < argc) {
            stride = std::stoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seeds.push_back(std::stoll(argv[++i]));
        } else if (arg == "--events") {
            events = true;
        } else {
            throw std::invalid_argument("unknown argument " + std::string(arg));
        }
    }
    if (stride < 1 || stride > 32) {
        throw std::invalid_argument("--stride must be 1..32");
    }
    if (seeds.empty()) {
        seeds.assign(kSeeds.begin(), kSeeds.end());
    }

    const auto overworld = stratum::data::ResourceLocation::parse("minecraft:overworld");
    const stratum::data::Pack pack = stratum::data::Pack::open(version / "worldgen");
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& settings = loaded.settings.at(overworld);
    if (!settings.aquifersEnabled) {
        throw std::runtime_error("the overworld's aquifers are off in this pack");
    }

    tiebreak::Tally total;
    for (const std::int64_t seed : seeds) {
        const std::filesystem::path region =
            version / "regions" / ("seed-" + std::to_string(seed)) / "overworld" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            throw std::runtime_error("no golden overworld region at " + region.string());
        }
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
        const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings);
        tiebreak::Replay replay(loaded.graph, noises, settings);
        std::vector<tiebreak::Event> recorded;
        replay.recordEvents(&recorded);
        stratum::test::GoldenRegion golden(region);

        tiebreak::Tally tally;
        for (std::int32_t cz = 0; cz < 32; cz += stride) {
            for (std::int32_t cx = 0; cx < 32; cx += stride) {
                if (!golden.hasChunk(cx, cz)) {
                    throw std::runtime_error("golden region lacks chunk " + std::to_string(cx) +
                                             " " + std::to_string(cz));
                }
                stratum::terrain::ChunkBuffer filled(settings.geometry);
                filler.fill(cx, cz, filled);
                replay.chunk(cx, cz, filled, golden, tally);
            }
        }
        const std::string label = "seed " + std::to_string(seed);
        print(label.c_str(), tally);
        if (events) {
            for (const tiebreak::Event& e : recorded) {
                std::printf(
                    "  event %s at %d %d %d D %.9f d %d %d %d %d status (%d,%d) (%d,%d) "
                    "(%d,%d) shipped %s swapped %s server %s level %d thresholds %.9f %.9f\n",
                    pairName(e.pair), e.x, e.y, e.z, e.density, e.distanceSq[0], e.distanceSq[1],
                    e.distanceSq[2], e.distanceSq[3], e.status[0].level,
                    static_cast<int>(e.status[0].type), e.status[1].level,
                    static_cast<int>(e.status[1].type), e.status[2].level,
                    static_cast<int>(e.status[2].type), nameOf(e.shipped), nameOf(e.swapped),
                    nameOf(e.server), e.serverLevel, e.rankedThreshold, e.swappedThreshold);
            }
        }
        std::fflush(stdout);
        total.absorb(tally);
    }
    print("total", total);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // A missing fixture, a malformed region or a bad argument surfaces as an
    // exception; report it and fail rather than terminate.
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
