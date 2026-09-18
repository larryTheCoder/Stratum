// Stratum — reads back tools/analysis/aps-boundary-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Two modes, and the second is what makes the first worth believing.
//
// `probe` scores candidate boundaries for `above_preliminary_surface` on a
// probe dimension. Every column of that world is solid from the floor up and
// its only surface rule paints a marker wherever the condition holds, so the
// LOWEST marker in a column is the condition's own boundary, exactly, and the
// scoring is a table of candidates against it rather than a fit. The
// candidates are the ones this project actually held or considered:
//
//     psl                          the reading this replaced, and SPEC's
//     psl + surfaceDepth - 8       what the sweep measured
//     trunc(psl) + surfaceDepth - 8  the same with a C-style cast for FLOOR
//     psl - (surfaceDepth + 3)     the leading hypothesis before the sweep,
//                                  which had the depth's sign backwards
//     psl - 6                      a flat offset, i.e. the modal column
//     psl + 8 + surfaceDepth       the depth's dependence with the wrong sign
//     psl + 2*surfaceDepth - 8     ... and with the wrong magnitude
//     psl + trunc(2.75*surface + 3) - 8   the depth WITHOUT its jitter draw
//     psl + max(0, surfaceDepth) - 8  a bottom clamp on the depth
//
// The last of those is reported but is NOT separated by any world here: the
// surface depth never went negative anywhere measured, so the clamp and its
// absence coincide on every column in hand. Separating them needs a datapack
// that overrides `minecraft:surface`'s own amplitudes, which this sweep does
// not build — said plainly rather than papered over.
//
// `golden` re-scores REAL regions, the way aquifer-waterlava-analyze.cpp
// re-scores the aquifer's: it counts the `grass_block`s the server itself
// placed BELOW `preliminary_surface_level`. A grass_block comes from the
// surface-materials subtree, and that subtree is exactly what this condition
// gates — it is `surface_rule.sequence[1]` in the overworld, the amplified
// and the large-biomes settings, and the condition appears nowhere else in
// any of vanilla's seven dimensions. So each of those blocks is one the old
// reading cannot place at all, and the measured band has to account for it.
// The residual it does not account for is reported rather than rounded away:
// a spatially VARYING psl reaches the condition sampled and interpolated
// rather than read per column (SPEC §11), which this build does not yet do.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/aps-boundary-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aps-boundary-analyze
//   build/aps-boundary-analyze probe  .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/probes/apsb/k_p0 0 .fixtures/1.21.11/probes/apsb/r_m0_5 -0.5
//   build/aps-boundary-analyze golden .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/regions/seed-42/overworld/r.0.0.mca
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace stratum;

namespace {

/// The vanilla pack at one seed: an interpreter over its density graph and a
/// compiled surface Executor, so every quantity here comes from the engine's
/// own code rather than from a replica written for the analysis.
class World {
public:
    World(const std::filesystem::path& tree, std::int64_t seed)
        : pack_(data::Pack::open(tree)), loaded_(settings::loadAll(pack_)),
          overworld_(loaded_.settings.at(data::ResourceLocation::parse("minecraft:overworld"))),
          rules_(surface::RuleGraph::resolve(overworld_.surfaceRule,
                                             data::ResourceLocation::parse("minecraft:overworld"))),
          noises_(density::NoiseRegistry::create(pack_, wanted(), seed,
                                                 density::RandomSource::Xoroshiro)),
          executor_(surface::Executor::compile(rules_, seed, overworld_.geometry, &noises_,
                                               overworld_.seaLevel)),
          interpreter_(loaded_.graph, noises_,
                       density::CellGeometry{.width = overworld_.geometry.cellWidth(),
                                             .height = overworld_.geometry.cellHeight()}) {}

    [[nodiscard]] std::int32_t surfaceDepth(std::int32_t x, std::int32_t z) const {
        return executor_.surfaceDepth(x, z);
    }

    [[nodiscard]] std::int32_t preliminarySurface(std::int32_t x, std::int32_t z) const {
        return static_cast<std::int32_t>(std::floor(
            interpreter_.evaluate(overworld_.router.at(settings::RouterEntry::PreliminarySurfaceLevel),
                                  density::Point{.x = x, .y = 0, .z = z})));
    }

    [[nodiscard]] const settings::NoiseGeometry& geometry() const { return overworld_.geometry; }

    /// The surface depth WITHOUT its `0.25 * nextDouble` jitter — a candidate
    /// in its own right, since a depth-shaped field that skipped the draw
    /// would agree with the real one on most columns and part from it on the
    /// rest.
    [[nodiscard]] std::int32_t depthWithoutJitter(std::int32_t x, std::int32_t z) const {
        const auto& field = noises_.get(data::ResourceLocation::parse("minecraft:surface"));
        return static_cast<std::int32_t>(
            (2.75 * field.sample(static_cast<double>(x), 0.0, static_cast<double>(z))) + 3.0);
    }

private:
    [[nodiscard]] std::vector<data::ResourceLocation> wanted() const {
        auto names = loaded_.graph.referencedNoises();
        const auto surfaceNoises = rules_.referencedNoises();
        names.insert(names.end(), surfaceNoises.begin(), surfaceNoises.end());
        names.push_back(data::ResourceLocation::parse("minecraft:surface"));
        names.push_back(data::ResourceLocation::parse("minecraft:surface_secondary"));
        names.push_back(data::ResourceLocation::parse("minecraft:clay_bands_offset"));
        return names;
    }

    data::Pack pack_;
    settings::LoadedSettings loaded_;
    settings::NoiseSettings overworld_;
    surface::RuleGraph rules_;
    density::NoiseRegistry noises_;
    surface::Executor executor_;
    density::Interpreter interpreter_;
};

/// One probe dimension, scored against every candidate.
void scoreProbe(const World& world, const std::filesystem::path& dir, double psl) {
    const std::filesystem::path region = dir / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(region)) {
        std::fprintf(stderr, "missing %s\n", region.c_str());
        return;
    }
    const auto file = region::RegionFile::open(region);
    const auto floored = static_cast<std::int32_t>(std::floor(psl));
    const auto truncated = static_cast<std::int32_t>(psl);

    long long columns = 0;
    long long broken = 0;
    std::map<std::string, long long> right;
    std::map<std::int32_t, long long> boundaries;
    // (surfaceDepth, floor(psl) - boundary). An anti-diagonal here is the
    // whole finding in one table.
    std::map<std::pair<std::int32_t, std::int32_t>, long long> joint;

    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk =
                chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    std::int32_t lowest = 0;
                    std::int32_t highest = 0;
                    long long marked = 0;
                    // The region holds far more chunks than the probe
                    // forceloaded; the rest stop at `biomes` or
                    // `structure_starts` with no marker anywhere, and
                    // scoring those would average in untouched terrain.
                    for (std::int32_t y = -64; y < 320; ++y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr || block->name != "minecraft:diamond_block") {
                            continue;
                        }
                        if (marked == 0) {
                            lowest = y;
                        }
                        highest = y;
                        ++marked;
                    }
                    if (marked == 0) {
                        continue;
                    }
                    ++columns;
                    if (highest - lowest + 1 != marked) {
                        ++broken;
                        continue;
                    }
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    const std::int32_t depth = world.surfaceDepth(x, z);
                    ++boundaries[lowest];
                    ++joint[{depth, floored - lowest}];
                    right["psl"] += static_cast<long long>(lowest == floored);
                    right["psl + depth - 8"] +=
                        static_cast<long long>(lowest == floored + depth - 8);
                    right["trunc(psl) + depth - 8"] +=
                        static_cast<long long>(lowest == truncated + depth - 8);
                    right["psl - (depth + 3)"] +=
                        static_cast<long long>(lowest == floored - (depth + 3));
                    right["psl - 6"] += static_cast<long long>(lowest == floored - 6);
                    right["psl + max(0, depth) - 8"] +=
                        static_cast<long long>(lowest == floored + std::max(0, depth) - 8);
                    right["psl + 8 + depth"] += static_cast<long long>(lowest == floored + 8 + depth);
                    right["psl + 2*depth - 8"] +=
                        static_cast<long long>(lowest == floored + (2 * depth) - 8);
                    right["psl + depth(no jitter) - 8"] += static_cast<long long>(
                        lowest == floored + world.depthWithoutJitter(x, z) - 8);
                }
            }
        }
    }

    std::printf("%s  psl=%g  columns=%lld  broken bands=%lld\n", dir.filename().c_str(), psl,
                columns, broken);
    for (const auto& [name, hits] : right) {
        std::printf("    %-24s %lld of %lld\n", name.c_str(), hits, columns);
    }
    std::printf("    boundary:");
    for (const auto& [value, count] : boundaries) {
        std::printf(" %d:%lld", value, count);
    }
    std::printf("\n    (depth, psl - boundary):");
    for (const auto& [key, count] : joint) {
        std::printf(" (%d,%d):%lld", key.first, key.second, count);
    }
    std::printf("\n");
}

/// One real region, scored the way the aquifer's re-scoring analyzer scores
/// the server's own blocks.
void scoreGolden(const World& world, const std::filesystem::path& region) {
    const auto file = region::RegionFile::open(region);
    const std::int32_t minY = world.geometry().minY;
    const std::int32_t topY = minY + world.geometry().height;

    long long grass = 0;
    long long belowOld = 0;
    long long insideBand = 0;
    std::map<std::int32_t, long long> offsets;

    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk =
                chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    for (std::int32_t y = minY; y < topY; ++y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr || block->name != "minecraft:grass_block") {
                            continue;
                        }
                        ++grass;
                        const std::int32_t psl = world.preliminarySurface(x, z);
                        if (y >= psl) {
                            continue;
                        }
                        ++belowOld;
                        ++offsets[y - psl];
                        insideBand +=
                            static_cast<long long>(y >= psl + world.surfaceDepth(x, z) - 8);
                    }
                }
            }
        }
    }

    std::printf("%s  grass_block=%lld  below psl=%lld  inside the band=%lld  below it=%lld\n",
                region.c_str(), grass, belowOld, insideBand, belowOld - insideBand);
    std::printf("    y - psl:");
    for (const auto& [offset, count] : offsets) {
        std::printf(" %d:%lld", offset, count);
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: aps-boundary-analyze probe  <worldgen> <seed> (<entry dir> <psl>)...\n"
                     "       aps-boundary-analyze golden <worldgen> <seed> <region.mca>...\n");
        return 2;
    }
    const std::string mode = argv[1];
    const World world{argv[2], std::strtoll(argv[3], nullptr, 10)};

    if (mode == "probe") {
        for (int arg = 4; arg + 1 < argc; arg += 2) {
            scoreProbe(world, argv[arg], std::strtod(argv[arg + 1], nullptr));
        }
        return 0;
    }
    if (mode == "golden") {
        for (int arg = 4; arg < argc; ++arg) {
            scoreGolden(world, argv[arg]);
        }
        return 0;
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
