// Stratum — the aquifer's THREE-source barrier, scored on real barriers.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `vanilla_aquifer_selection_test.cpp` scores the old two-source-only
// `placesBarrier` and states plainly that it is "refuted as a general
// model" (SPEC §11): no density term, no third source, missing 11-18% of
// the server's real barrier blocks outright. This scores the replacement —
// `BarrierAt` now carries the caller's density and a third ranked source —
// directly against the same kind of real barrier data, and specifically
// against what the OLD rule alone still misses.
//
// UNLIKE every other aquifer conformance fixture in this project, this one
// needs REAL `barrier`/`fluid_level_floodedness`/`fluid_level_spread`
// noise, not a constant or a synthetic field: a genuine third-source
// junction only shows up where cell-to-cell variation is real (SPEC §11,
// MA blocker 3). `tools/analysis/aquifer-barrier-probe.sh` builds that
// world; this test reads it the same way
// `tools/analysis/aquifer-barrier-analyze.cpp` does — real noise through
// this build's own density::Interpreter, not a hand-rolled replica — and
// calls the SAME committed `placesBarrier` twice per block: once with all
// three ranked sources, once with the third pushed far enough away to be
// inert, isolating exactly what the third source changes.
//
// ONE SEED, READ FROM THE PROBE'S OWN MANIFEST rather than assumed. Every
// dimension in a `density-probe.sh` spec shares one world and one seed, and
// the output directory is named after the spec, not the seed — so unlike
// `vanilla_aquifer_selection_test.cpp`'s four separately-named worlds, only
// whichever seed a developer last ran this probe with is ever on disk at
// once. Reading `manifest.json` rather than hardcoding a seed is what makes
// this correct regardless of which one that was, instead of silently
// scoring the wrong world against the right seed's math.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
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

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace stratum;

constexpr std::int32_t kSeaLevel = 63;
const aquifer::PslRead kSurface = aquifer::constantSurface(96);
constexpr std::int64_t kInertDistanceSq = 1'000'000;
constexpr std::int32_t kMinY = -48;
constexpr std::int32_t kMaxY = 271; // MIN_Y(-48) + HEIGHT(320) - 1, aquifer-barrier-probe.sh
constexpr std::int32_t kChunks = 8;

struct Dimension {
    const char* name;
    double density;
};

// Must match aquifer-barrier-probe.sh's own `aquifer(name, density)` calls.
constexpr std::array<Dimension, 3> kDimensions{
    {{"d_neg0_3", -0.3}, {"d_neg1_0", -1.0}, {"d_neg3_0", -3.0}}};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
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

struct Score {
    long long realBarriers = 0;
    long long twoSourceMisses = 0;
    long long threeSourceMisses = 0;
};

} // namespace

TEST_CASE("the three-source barrier explains real barriers the two-source rule misses",
          "[conformance][aquifer]") {
    const std::filesystem::path probeDir = fixtures() / "probes" / "barrier3way";
    const std::filesystem::path manifestPath = probeDir / "manifest.json";
    if (!std::filesystem::is_regular_file(manifestPath)) {
        SKIP("no barrier3way aquifer probe at " << probeDir
                                                << "; generate it with "
                                                   "tools/analysis/aquifer-barrier-probe.sh");
    }
    std::ifstream manifestFile(manifestPath);
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    const std::int64_t seed = manifest.at("seed").get<std::int64_t>();

    const auto pack = data::Pack::open(fixtures() / "worldgen");
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

    Score total;
    for (const Dimension& dim : kDimensions) {
        const std::filesystem::path region = probeDir / dim.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const auto file = region::RegionFile::open(region);
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        for (std::int32_t y = kMinY; y <= kMaxY; ++y) {
                            const auto* b = ch.blockAt(lx, y, lz);
                            if (b == nullptr || b->name != "minecraft:stone") {
                                continue; // Only real barriers are this test's to score.
                            }
                            ++total.realBarriers;

                            const aquifer::Selection selection =
                                aquifer::selectSources(centres, x, y, z);
                            std::array<aquifer::BarrierSource, 3> ranked{};
                            for (int r = 0; r < 3; ++r) {
                                const auto& src = selection.ranked[static_cast<std::size_t>(r)];
                                const double flood = interp.evaluate(
                                    floodNode,
                                    density::Point{
                                        .x = src.centre.x, .y = src.centre.y, .z = src.centre.z},
                                    cache);
                                const aquifer::SamplePos pos =
                                    aquifer::spreadSample(src.cell, src.centre);
                                const double spread = interp.evaluate(
                                    spreadNode, density::Point{.x = pos.x, .y = pos.y, .z = pos.z},
                                    cache);
                                ranked[static_cast<std::size_t>(r)] = sourceOf(src, flood, spread);
                            }
                            const double barrierNoise = interp.evaluate(
                                barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);

                            aquifer::BarrierAt at;
                            at.y = y;
                            at.density = dim.density;
                            at.nearest = ranked[0];
                            at.second = ranked[1];
                            at.third = ranked[2];
                            at.barrier = barrierNoise;

                            if (!aquifer::placesBarrier(at)) {
                                ++total.threeSourceMisses;
                            }
                            aquifer::BarrierAt twoSourceOnly = at;
                            twoSourceOnly.third.distanceSq = kInertDistanceSq;
                            if (!aquifer::placesBarrier(twoSourceOnly)) {
                                ++total.twoSourceMisses;
                            }
                        }
                    }
                }
            }
        }
    }

    REQUIRE(total.realBarriers > 2000);

    INFO("seed " << seed << ", real barriers " << total.realBarriers << ", two-source misses "
                 << total.twoSourceMisses << ", three-source misses " << total.threeSourceMisses);

    // The control: the old two-source-only rule really is still measurably
    // incomplete on real barriers (SPEC §11 measured 11.5-17.6% per
    // dimension) — if this ever reads near zero, the control has broken,
    // not the finding.
    CHECK(total.twoSourceMisses * 100 > total.realBarriers * 8);

    // The three-source rule: SPEC §11 measured 0.4-2.4% per dimension,
    // 1.01-1.02% overall across two seeds. The bound is set well above that
    // so it flags a real regression rather than seed-to-seed drift.
    CHECK(total.threeSourceMisses * 100 < total.realBarriers * 5);
}
