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
    long long openBesideBarrier = 0;
    long long falseStoneBesideBarrier = 0;
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
        // The whole three-source answer at one block, as the filler computes it.
        const auto barrierAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            const aquifer::Selection selection = aquifer::selectSources(centres, x, y, z);
            std::array<aquifer::BarrierSource, 3> ranked{};
            for (int r = 0; r < 3; ++r) {
                const auto& src = selection.ranked[static_cast<std::size_t>(r)];
                const double flood = interp.evaluate(
                    floodNode,
                    density::Point{.x = src.centre.x, .y = src.centre.y, .z = src.centre.z}, cache);
                const aquifer::SamplePos pos = aquifer::spreadSample(src.cell, src.centre);
                const double spread = interp.evaluate(
                    spreadNode, density::Point{.x = pos.x, .y = pos.y, .z = pos.z}, cache);
                ranked[static_cast<std::size_t>(r)] = sourceOf(src, flood, spread);
            }
            aquifer::BarrierAt at;
            at.y = y;
            at.density = dim.density;
            at.nearest = ranked[0];
            at.second = ranked[1];
            at.third = ranked[2];
            at.barrier =
                interp.evaluate(barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);
            return at;
        };
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                // A probe region holds every chunk of its window: a missing one is
                // a broken corpus, not a smaller sample.
                REQUIRE(file.hasChunk(cx, cz));
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

                            const aquifer::BarrierAt at = barrierAt(x, y, z);

                            if (!aquifer::placesBarrier(at)) {
                                ++total.threeSourceMisses;
                            }
                            // False stone where a wrong model would put it
                            // first: the open blocks right beside a real
                            // barrier, two either side vertically.
                            for (const std::int32_t dy : {-2, -1, 1, 2}) {
                                const std::int32_t ny = y + dy;
                                if (ny < kMinY || ny > kMaxY) {
                                    continue;
                                }
                                const auto* next = ch.blockAt(lx, ny, lz);
                                if (next == nullptr || next->name == "minecraft:stone") {
                                    continue;
                                }
                                ++total.openBesideBarrier;
                                if (aquifer::placesBarrier(barrierAt(x, ny, z))) {
                                    ++total.falseStoneBesideBarrier;
                                }
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

    INFO("seed " << seed << ", real barriers " << total.realBarriers << ", two-source misses "
                 << total.twoSourceMisses << ", three-source misses " << total.threeSourceMisses
                 << ", open blocks beside a barrier " << total.openBesideBarrier
                 << ", false stone there " << total.falseStoneBesideBarrier);
    // The corpus itself, exactly: a missing dimension or chunk changes it.
    REQUIRE(total.realBarriers == 11923);

    // The control: the old two-source-only rule really is still measurably
    // incomplete on real barriers (SPEC §11 measured 11.5-17.6% per
    // dimension) — if this ever reads near zero, the control has broken,
    // not the finding.
    CHECK(total.twoSourceMisses * 100 > total.realBarriers * 8);

    // The three-source rule: EXACT. It read 121 misses (1.015%) before the
    // dry sentinel and the unclamped ladder, 6 after them, and 0 of 11 923
    // since the barrier's `/10` floor lost its agree-guard — on all three
    // densities. A rate bound had 100x slack by then and flagged nothing; a
    // corpus this fixed is held exactly.
    CHECK(total.threeSourceMisses == 0);
    // And the other direction, never checked before: no stone where the
    // server left the block open, on every open block within two of a real
    // barrier vertically — where a shifted or thickened barrier would land.
    REQUIRE(total.openBesideBarrier > 10000);
    CHECK(total.falseStoneBesideBarrier == 0);
}
