// Stratum — dumps every ore-vein RNG "candidate" position (the confirmed
// deterministic gate cleared: y-range, sign/type, richness, and
// vein_ridged < 0) from tools/analysis/ore-vein-probe.sh's worlds, with
// whether the server actually touched it (filler/ore) or left it stone
// (the roll failed).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The 30% membership roll is the first of ore veins' three random draws
// (SPEC's M3 section, "ore veins... the RNG still open") and the most
// tractable to isolate: `vein_ridged < 0` is a confirmed NECESSARY
// condition (0 exceptions across 25608 real blocks) — but NOT a sufficient
// one to filter candidates by on its own. `vein_ridged`'s own formula is
// `-0.08 + max(|a|, |b|)`, and OUTSIDE the vein y-ranges both `a` and `b`
// range_choice to 0, leaving `vein_ridged = -0.08 < 0` there too — a first
// version of this tool filtered on `ridged < 0` alone and pulled in 4.47M
// "candidates" outside the range where the real algorithm never even
// considers the roll, diluting the touched rate to 0.008% instead of
// anywhere near 30%. Candidates here are gated the same way real vein
// blocks were already confirmed to be: y in range, `vein_toggle`'s sign
// matching the sub-range's type, AND clearing the richness threshold —
// exactly the deterministic gate `ore-vein-analyze.cpp` found zero
// exceptions to.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/ore-vein-rng-dump.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/ore-vein-rng-dump
//   build/ore-vein-rng-dump .fixtures/1.21.11/probes/orevein-multi out.csv <seed1> <seed2> ...
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace stratum;

namespace {
constexpr int kMinY = -64;
constexpr int kMaxY = 319;

/// The confirmed deterministic gate: is `y`/`toggle` a candidate position
/// for the vein system at all, whatever `vein_ridged` says? Mirrors
/// ore-vein-analyze.cpp's own `signOk`/richness check exactly.
bool typeAndRichnessOk(const int y, const double toggle) {
    const bool inCopperRange = y >= 0 && y <= 50;
    const bool inIronRange = y >= -60 && y <= -8;
    const bool isCopper = inCopperRange && toggle > 0.0;
    const bool isIron = inIronRange && toggle <= 0.0;
    if (!isCopper && !isIron) {
        return false;
    }
    const int lower = isCopper ? 0 : -60;
    const int upper = isCopper ? 50 : -8;
    const int distFromLimit = std::min(y - lower, upper - y);
    const double required = 0.6 - (0.2 * std::min(distFromLimit, 20) / 20.0);
    return std::abs(toggle) >= required;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: ore-vein-rng-dump <probe-root> <out.csv> <seed> [<seed> ...]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path outPath = argv[2];
    const std::filesystem::path fixturesWorldgen = root.parent_path().parent_path() / "worldgen";

    const auto pack = data::Pack::open(fixturesWorldgen);
    const auto loaded = settings::loadAll(pack);
    const auto& overworld = loaded.settings.at(data::ResourceLocation::parse("minecraft:overworld"));
    const auto toggleNodeIdx = overworld.router.at(settings::RouterEntry::VeinToggle);
    const auto ridgedNodeIdx = overworld.router.at(settings::RouterEntry::VeinRidged);

    std::FILE* out = std::fopen(outPath.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot open %s for writing\n", outPath.c_str());
        return 1;
    }
    std::fprintf(out, "seed,x,y,z,ridged,touched\n");

    long long totalCandidates = 0;
    long long totalTouched = 0;

    for (int argi = 3; argi < argc; ++argi) {
        const std::int64_t seed = std::atoll(argv[argi]);
        const std::filesystem::path region =
            root / ("seed-" + std::to_string(seed)) / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            std::fprintf(stderr, "skip: no region at %s\n", region.c_str());
            continue;
        }
        const auto noises = density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, density::RandomSource::Xoroshiro);
        density::Interpreter interp(
            loaded.graph, noises,
            density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                  .height = overworld.geometry.cellHeight()});
        density::Interpreter::CornerCache cache(interp.cacheSize());

        const auto file = region::RegionFile::open(region);
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
                        // Only y in [-60, 50] can ever be a candidate — skip
                        // the rest outright rather than evaluating toggle
                        // there at all.
                        for (int y = -60; y <= 50; ++y) {
                            const double toggle = interp.evaluate(
                                toggleNodeIdx, density::Point{.x = x, .y = y, .z = z}, cache);
                            if (!typeAndRichnessOk(y, toggle)) {
                                continue;
                            }
                            const double ridged = interp.evaluate(
                                ridgedNodeIdx, density::Point{.x = x, .y = y, .z = z}, cache);
                            if (ridged >= 0.0) {
                                continue;
                            }
                            const auto* b = ch.blockAt(lx, y, lz);
                            const bool touched = b != nullptr && b->name != "minecraft:stone";
                            ++totalCandidates;
                            if (touched) {
                                ++totalTouched;
                            }
                            std::fprintf(out, "%lld,%d,%d,%d,%.10f,%d\n",
                                        static_cast<long long>(seed), x, y, z, ridged,
                                        touched ? 1 : 0);
                        }
                    }
                }
            }
        }
        std::fprintf(stderr, "seed %lld done (running total: %lld candidates, %lld touched)\n",
                    static_cast<long long>(seed), totalCandidates, totalTouched);
    }
    std::fclose(out);
    std::fprintf(stderr, "wrote %lld candidate rows (%lld touched, %.3f%%) to %s\n",
                totalCandidates, totalTouched,
                100.0 * double(totalTouched) / double(totalCandidates ? totalCandidates : 1),
                outPath.c_str());
    return 0;
}
