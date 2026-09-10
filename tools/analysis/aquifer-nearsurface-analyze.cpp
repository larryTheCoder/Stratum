// Stratum — reads back tools/analysis/aquifer-nearsurface-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Replicates the probe's own `preliminary_surface_level` field in C++ (the
// same noise, the same xz_scale, the same two thresholds), then for EVERY
// block in the probed chunks — not per fluid body, per block — finds its
// deciding cell (selection.hpp's nearest of four ranked candidates) and
// calls the real `cellFluidLevel` twice: once as measured, once with one
// field of the surface toggled to the rival reading. `y < level` is the
// prediction; whether the server actually placed a fluid there is the
// readout. Comparing both predictions against that is the whole experiment
// — see the probe script's own header for what each of the three subsets
// tests.
//
// PER-BLOCK, NOT PER-BODY, and that correction is itself a finding worth
// keeping on record. A first version of this tool grouped contiguous
// fluid runs into "bodies" and compared only the top boundary — the same
// technique every earlier aquifer probe in this project used successfully.
// It scored 0.51 here, next to worthless, and the reason was not a wrong
// hypothesis: water meeting lava turns the contact block to obsidian, which
// is neither `minecraft:water` nor `minecraft:lava`, so a single-block
// water/lava boundary (extremely common under this probe's own extreme psl
// swings) silently split one continuous column into two fake "bodies" with
// a fabricated air gap between them. Reading fluid/air at every y directly,
// and skipping only the handful of blocks that are neither, has no such
// failure mode.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       tools/analysis/aquifer-nearsurface-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aquifer-nearsurface-analyze
//   build/aquifer-nearsurface-analyze .fixtures/1.21.11/probes/nearsurface <seed>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace stratum;

namespace {

constexpr int kMinY = -64;
constexpr int kMaxY = 319;
constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kOceanGate = kSeaLevel - aquifer::kOceanGateOffset;
constexpr std::int32_t kLambda = aquifer::lambdaLevel(kSeaLevel);
constexpr double kFloodedness = 0.9;
constexpr double kSpread = 0.0;

constexpr double kTLow = -0.25;
constexpr double kTHigh = 0.25;
constexpr double kLow = -70.0;
constexpr double kMid = -20.0;
constexpr double kHigh = 96.0;

struct Tally {
    long long total = 0;
    long long matchCurrent = 0;
    long long matchAlt = 0;
    long long matchBoth = 0;
    long long matchNeither = 0;
};

void record(Tally& t, bool observedWet, bool currentWet, bool altWet) {
    ++t.total;
    const bool mc = observedWet == currentWet;
    const bool ma = observedWet == altWet;
    if (mc) {
        ++t.matchCurrent;
    }
    if (ma) {
        ++t.matchAlt;
    }
    if (mc && ma) {
        ++t.matchBoth;
    }
    if (!mc && !ma) {
        ++t.matchNeither;
    }
}

void report(const char* label, const Tally& t) {
    const double pc =
        t.total ? static_cast<double>(t.matchCurrent) / static_cast<double>(t.total) : 0.0;
    const double pa =
        t.total ? static_cast<double>(t.matchAlt) / static_cast<double>(t.total) : 0.0;
    std::printf("  %-14s total=%-7lld current=%-7lld (%.4f)  alt=%-7lld (%.4f)  both=%-7lld  "
                "neither=%-7lld\n",
                label, t.total, t.matchCurrent, pc, t.matchAlt, pa, t.matchBoth, t.matchNeither);
}

/// Which xz_scale each probe dimension's field was built with — must match
/// aquifer-nearsurface-probe.sh's own `aquifer(name, scale)` calls exactly.
double scaleForDimension(const std::string& name) {
    if (name == "nsf_8") {
        return 1.0;
    }
    if (name == "nsf_16") {
        return 0.5;
    }
    std::fprintf(stderr, "unknown dimension %s, skipping\n", name.c_str());
    return 0.0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: aquifer-nearsurface-analyze <probe-dir> <seed>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::int64_t seed = std::atoll(argv[2]);

    // A throwaway datapack holding only the noise the probe's own psl field
    // reads — a single octave, so nothing about the replica is in doubt.
    const std::filesystem::path packDir =
        std::filesystem::temp_directory_path() / "stratum-nsf-analyze-pack";
    std::filesystem::create_directories(packDir / "data" / "stratum" / "worldgen" / "noise");
    {
        std::ofstream meta(packDir / "pack.mcmeta");
        meta << R"({"pack": {"pack_format": 94, "description": "nsf analyze"}})";
    }
    {
        std::ofstream noiseFile(packDir / "data" / "stratum" / "worldgen" / "noise" /
                                 "probe_noise.json");
        noiseFile << R"({"firstOctave": -3, "amplitudes": [1.0]})";
    }
    const auto pack = data::Pack::open(packDir);
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("stratum:probe_noise")};
    const auto noises =
        density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
    const auto& probeNoise = noises.get(wanted[0]);
    const aquifer::CentreSource centres(seed);

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());

    Tally floorTally;
    Tally oceanTally;
    Tally depthTally;
    Tally baseline; // sanity: current code vs observed, over every WET/AIR block.
    long long skipped = 0;

    for (const std::string& name : names) {
        const double xzScale = scaleForDimension(name);
        if (xzScale == 0.0) {
            continue;
        }
        const auto psl = [&](std::int32_t x, std::int32_t /*y*/, std::int32_t z) -> double {
            const double n = probeNoise.sample(static_cast<double>(x) * xzScale, 0.0,
                                                static_cast<double>(z) * xzScale);
            if (n < kTLow) {
                return kLow;
            }
            if (n < kTHigh) {
                return kMid;
            }
            return kHigh;
        };

        const std::filesystem::path regionPath = root / name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(regionPath)) {
            continue;
        }
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
                        for (int y = kMinY; y <= kMaxY; ++y) {
                            const auto* b = ch.blockAt(lx, y, lz);
                            bool observedWet;
                            if (b && b->name == "minecraft:water") {
                                observedWet = true;
                            } else if (b && b->name == "minecraft:lava") {
                                observedWet = true;
                            } else if (b && b->name == "minecraft:air") {
                                observedWet = false;
                            } else {
                                // Water/lava contact converts to obsidian (or
                                // similar) a tick after worldgen, and a
                                // suppressed-but-not-eliminated barrier can
                                // still leave the rare stone block (SPEC's
                                // own ~13% unmodeled third source). Neither
                                // says anything about the LEVEL rule, so ambiguous
                                // blocks are skipped rather than mis-scored.
                                ++skipped;
                                continue;
                            }

                            // The DECIDING cell is the nearest of the ranked
                            // candidates (selection.hpp), not necessarily
                            // the lattice's own "home" cell for this
                            // position — selectSources replicates vanilla's
                            // own window-based nearest search, already
                            // scored at 0.99993 on the server's own barrier
                            // blocks.
                            const aquifer::Selection selection =
                                aquifer::selectSources(centres, x, y, z);
                            const aquifer::CellIndex centre = selection.nearest().centre;
                            const aquifer::PslRead surface =
                                aquifer::readPreliminarySurface(psl, centre, kSeaLevel);

                            aquifer::CellFluid cellFluid{.centreY = centre.y,
                                                          .surface = surface,
                                                          .seaLevel = kSeaLevel,
                                                          .floodedness = kFloodedness,
                                                          .spread = kSpread};
                            const std::int32_t current = aquifer::cellFluidLevel(cellFluid);
                            const bool currentWet = y < current;
                            record(baseline, observedWet, currentWet, currentWet);

                            const bool nearSurface =
                                surface.gate < kOceanGate &&
                                (surface.gate - centre.y) < aquifer::kNearSurfaceDepth;

                            if (nearSurface) {
                                if (surface.aborted && surface.gate != surface.cap) {
                                    aquifer::CellFluid altCell = cellFluid;
                                    altCell.surface.cap = surface.gate;
                                    const std::int32_t alt = aquifer::cellFluidLevel(altCell);
                                    record(floorTally, observedWet, currentWet, y < alt);
                                }
                                continue;
                            }

                            if (!surface.aborted || centre.y < kLambda) {
                                continue;
                            }
                            aquifer::CellFluid altCell = cellFluid;
                            altCell.surface.aborted = false;
                            const std::int32_t alt = aquifer::cellFluidLevel(altCell);
                            if (surface.anchor < kOceanGate) {
                                record(depthTally, observedWet, currentWet, y < alt);
                            } else {
                                record(oceanTally, observedWet, currentWet, y < alt);
                            }
                        }
                    }
                }
            }
        }
    }

    std::printf("skipped (neither fluid nor air): %lld\n\n", skipped);
    std::printf("baseline (current code vs observed, every block):\n");
    report("all", baseline);
    std::printf("\n(a) near-surface floor, cap (current) vs gate (alt):\n");
    report("floor", floorTally);
    std::printf("\n(b) ocean branch sea check, aborted (current) vs ignored (alt):\n");
    report("ocean", oceanTally);
    std::printf("\n(c) depth path sea check, aborted (current) vs ignored (alt):\n");
    report("depth", depthTally);
    return 0;
}
