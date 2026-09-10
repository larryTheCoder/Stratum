// Stratum — reads back tools/analysis/aquifer-barrier-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// For every stone/water/air block, ranks the four nearest aquifer sources
// (selection.hpp), reads `barrier`/`fluid_level_floodedness`/
// `fluid_level_spread` through the REAL vanilla noises via this build's own
// density::Interpreter — not a hand-rolled replica, to remove that whole
// class of risk — and calls this build's own `placesBarrier` (barrier.hpp)
// TWICE per block: once with the real three ranked sources, once with the
// third pushed far enough away to be inert, isolating exactly what the third
// source changes. Both calls go through the same committed function, so
// this is an end-to-end check of the shipped code, not a parallel
// reimplementation of it.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/aquifer-barrier-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aquifer-barrier-analyze
//   build/aquifer-barrier-analyze .fixtures/1.21.11/probes/barrier3way <seed>
#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace stratum;

namespace {

constexpr int kMaxY = 271; // MIN_Y(-48) + HEIGHT(320) - 1
constexpr std::int32_t kSeaLevel = 63;
const aquifer::PslRead kSurface = aquifer::constantSurface(96);
// Pushes a source's s13/s23 to <= 0 regardless of the other two, without
// touching the ranking itself.
constexpr std::int64_t kInertDistanceSq = 1'000'000;

/// Which raw_final_density constant each probe dimension used — must match
/// aquifer-barrier-probe.sh's own `aquifer(name, density)` calls exactly.
double densityForDimension(const std::string& name) {
    if (name == "d_neg1_0") {
        return -1.0;
    }
    if (name == "d_neg0_3") {
        return -0.3;
    }
    if (name == "d_neg3_0") {
        return -3.0;
    }
    std::fprintf(stderr, "unknown dimension %s, skipping\n", name.c_str());
    return 1.0; // positive: never reached by the aquifer (Q2.2), so skip-safe.
}

aquifer::BarrierSource sourceOf(const aquifer::Source& ranked, double floodedness, double spread) {
    aquifer::CellFluid cell{.centreY = ranked.centre.y,
                             .surface = kSurface,
                             .seaLevel = kSeaLevel,
                             .floodedness = floodedness,
                             .spread = spread};
    return aquifer::BarrierSource{.level = aquifer::cellFluidLevel(cell),
                                   .distanceSq = ranked.distanceSq};
}

struct Tally {
    long long total = 0;
    long long matchTwoSource = 0;
    long long matchThreeSource = 0;
    long long matchBoth = 0;
    long long matchNeither = 0;
    // Cases the two-source rule gets WRONG that the three-source rule gets
    // RIGHT — the 13% this experiment is chasing.
    long long thirdSourceRescues = 0;
    // Restricted to blocks the server actually made SOLID — SPEC's own "13%
    // of the server's real barriers" framing, rather than the error rate
    // over every block (which a huge non-barrier majority dilutes).
    long long realBarriers = 0;
    long long realBarriersTwoSourceMisses = 0;
    long long realBarriersThreeSourceMisses = 0;
};

void record(Tally& t, bool observedSolid, bool twoSourceSolid, bool threeSourceSolid) {
    ++t.total;
    const bool m2 = observedSolid == twoSourceSolid;
    const bool m3 = observedSolid == threeSourceSolid;
    if (m2) {
        ++t.matchTwoSource;
    }
    if (m3) {
        ++t.matchThreeSource;
    }
    if (m2 && m3) {
        ++t.matchBoth;
    }
    if (!m2 && !m3) {
        ++t.matchNeither;
    }
    if (!m2 && m3) {
        ++t.thirdSourceRescues;
    }
    if (observedSolid) {
        ++t.realBarriers;
        if (!twoSourceSolid) {
            ++t.realBarriersTwoSourceMisses;
        }
        if (!threeSourceSolid) {
            ++t.realBarriersThreeSourceMisses;
        }
    }
}

void report(const Tally& t) {
    const double p2 =
        t.total ? static_cast<double>(t.matchTwoSource) / static_cast<double>(t.total) : 0.0;
    const double p3 =
        t.total ? static_cast<double>(t.matchThreeSource) / static_cast<double>(t.total) : 0.0;
    std::printf("  total=%-8lld two-source=%-7lld (%.5f)  three-source=%-7lld (%.5f)  "
                "both=%-7lld  neither=%-7lld  rescued=%-7lld\n",
                t.total, t.matchTwoSource, p2, t.matchThreeSource, p3, t.matchBoth,
                t.matchNeither, t.thirdSourceRescues);
    const double missRate2 = t.realBarriers ? 100.0 *
                                                   static_cast<double>(t.realBarriersTwoSourceMisses) /
                                                   static_cast<double>(t.realBarriers)
                                             : 0.0;
    const double missRate3 = t.realBarriers
                                  ? 100.0 * static_cast<double>(t.realBarriersThreeSourceMisses) /
                                        static_cast<double>(t.realBarriers)
                                  : 0.0;
    std::printf("  real barriers=%-8lld two-source misses=%-6lld (%.3f%%)  "
                "three-source misses=%-6lld (%.3f%%)\n",
                t.realBarriers, t.realBarriersTwoSourceMisses, missRate2,
                t.realBarriersThreeSourceMisses, missRate3);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: aquifer-barrier-analyze <probe-dir> <seed>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::int64_t seed = std::atoll(argv[2]);

    const char* fixturesEnv = std::getenv("STRATUM_FIXTURES_DIR");
    const std::filesystem::path fixturesDir =
        (fixturesEnv ? std::filesystem::path(fixturesEnv) : std::filesystem::path(".fixtures")) /
        "1.21.11";
    const auto pack = data::Pack::open(fixturesDir / "worldgen");

    density::Graph::Builder builder(pack);
    const nlohmann::json barrierJson = {{"type", "minecraft:noise"},
                                         {"noise", "minecraft:aquifer_barrier"},
                                         {"xz_scale", 1.0},
                                         {"y_scale", 0.5}};
    const nlohmann::json floodJson = {{"type", "minecraft:noise"},
                                       {"noise", "minecraft:aquifer_fluid_level_floodedness"},
                                       {"xz_scale", 1.0},
                                       {"y_scale", 0.67}};
    const nlohmann::json spreadJson = {{"type", "minecraft:noise"},
                                        {"noise", "minecraft:aquifer_fluid_level_spread"},
                                        {"xz_scale", 1.0},
                                        {"y_scale", 0.7142857142857143}};
    const density::NodeIndex barrierNode = builder.add(barrierJson);
    const density::NodeIndex floodNode = builder.add(floodJson);
    const density::NodeIndex spreadNode = builder.add(spreadJson);
    const density::Graph graph = builder.release();

    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_floodedness"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};
    const auto noises =
        density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
    const density::Interpreter interp(graph, noises);
    density::Interpreter::CornerCache cache(interp.cacheSize());

    const aquifer::CentreSource centres(seed);

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());

    Tally overall;
    long long skipped = 0;

    for (const std::string& name : names) {
        const double density = densityForDimension(name);
        if (density > 0.0) {
            continue;
        }
        const std::filesystem::path regionPath = root / name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(regionPath)) {
            continue;
        }
        std::printf("=== %s (D=%.2f) ===\n", name.c_str(), density);
        Tally perDimension;
        const auto file = region::RegionFile::open(regionPath);
        for (std::int32_t cz = 0; cz < 8; ++cz) {
            for (std::int32_t cx = 0; cx < 8; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        const int x = cx * 16 + lx;
                        const int z = cz * 16 + lz;
                        for (int y = -48; y <= kMaxY; ++y) {
                            const auto* b = ch.blockAt(lx, y, lz);
                            bool observedSolid;
                            if (b && b->name == "minecraft:water") {
                                observedSolid = false;
                            } else if (b && b->name == "minecraft:air") {
                                observedSolid = false;
                            } else if (b && b->name == "minecraft:stone") {
                                observedSolid = true;
                            } else {
                                ++skipped;
                                continue;
                            }

                            const aquifer::Selection selection =
                                aquifer::selectSources(centres, x, y, z);
                            const auto floodednessAt = [&](const aquifer::CellIndex& c) {
                                return interp.evaluate(
                                    floodNode, density::Point{.x = c.x, .y = c.y, .z = c.z},
                                    cache);
                            };
                            const auto spreadAt = [&](const aquifer::CellIndex& cell,
                                                       const aquifer::CellIndex& c) {
                                const aquifer::SamplePos pos = aquifer::spreadSample(cell, c);
                                return interp.evaluate(
                                    spreadNode,
                                    density::Point{.x = pos.x, .y = pos.y, .z = pos.z}, cache);
                            };

                            std::array<aquifer::BarrierSource, 3> ranked{};
                            for (int r = 0; r < 3; ++r) {
                                const auto& src = selection.ranked[static_cast<std::size_t>(r)];
                                const double flood = floodednessAt(src.centre);
                                const double spread = spreadAt(src.cell, src.centre);
                                ranked[static_cast<std::size_t>(r)] = sourceOf(src, flood, spread);
                            }

                            const double barrierNoise = interp.evaluate(
                                barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);

                            aquifer::BarrierAt at;
                            at.y = y;
                            at.density = density;
                            at.nearest = ranked[0];
                            at.second = ranked[1];
                            at.third = ranked[2];
                            at.barrier = barrierNoise;
                            const bool threeSourceSolid = aquifer::placesBarrier(at);

                            aquifer::BarrierAt twoSourceOnly = at;
                            twoSourceOnly.third.distanceSq = kInertDistanceSq;
                            const bool twoSourceSolid = aquifer::placesBarrier(twoSourceOnly);

                            record(perDimension, observedSolid, twoSourceSolid, threeSourceSolid);
                        }
                    }
                }
            }
        }
        report(perDimension);
        overall.total += perDimension.total;
        overall.matchTwoSource += perDimension.matchTwoSource;
        overall.matchThreeSource += perDimension.matchThreeSource;
        overall.matchBoth += perDimension.matchBoth;
        overall.matchNeither += perDimension.matchNeither;
        overall.thirdSourceRescues += perDimension.thirdSourceRescues;
        overall.realBarriers += perDimension.realBarriers;
        overall.realBarriersTwoSourceMisses += perDimension.realBarriersTwoSourceMisses;
        overall.realBarriersThreeSourceMisses += perDimension.realBarriersThreeSourceMisses;
    }

    std::printf("\nskipped (neither stone, water, nor air): %lld\n\n", skipped);
    std::printf("=== overall ===\n");
    report(overall);
    return 0;
}
