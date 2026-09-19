// Stratum — reads back tools/analysis/aps-boundary-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Eight modes. `probe` is the measurement; the rest are what make it worth
// believing — the control that it is the condition rather than the surface
// pass, both directions of the golden re-scoring, the census that says which
// candidates a world can still separate, and the three (`sweep`, `window`,
// `clamp`) that close the bottom-clamp question by going and finding the
// columns that separate it rather than waiting for one to turn up.
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
// The last of those is now REFUTED, and the three modes that did it are the
// reason. The two candidates differ only where the RETURNED depth is
// negative, which needs a raw value at or below -1 because the cast truncates
// toward zero. `depth` is the census over a real region: across 2097152
// columns — every column of all eight golden overworld regions — the returned
// depth is never negative, and the lowest raw value is -0.449658 (seed 0; the
// other seven never go below zero at all). That is a fact about those
// regions, not about vanilla, and two earlier versions of this header drew
// universal conclusions from samples that could not carry them.
//
// `sweep` is the fix for that: pure field evaluation, parallel, ~6.85M
// columns/sec on twelve threads, reporting a RATE with its denominator, its
// window and its seeds. Over the eight golden seeds and x, z in
// [-16384, 16384) — 1073741824 columns each, 8589934592 in all — 98 columns
// reach depth -1, on six of the eight seeds, in ten distinct regions: 1 in
// 87652393. It prints every one, plus the raw tail in 0.01 bands so the
// crossing columns can be seen to be the distribution's continuation rather
// than its edge.
//
// `window` reads the one disagreed-about block in a real overworld and is
// the mode that shows why that is NOT enough: at the first known cluster it
// returns stone under both candidates, because those columns are warm_ocean
// with y = psl - 9 buried 21 blocks deep, where every arm of the gated
// subtree declines at surfaceDepth -1 (see its own comment).
//
// `clamp` is the separation. `aps-clamp-probe.sh` builds probe dimensions
// with nothing between the condition and the readout, pointed at where the
// negative columns actually are, and `clamp` scores them: the band's lower
// edge is `psl + surfaceDepth - 8` on all 49 separating columns across three
// seeds, never `psl + max(0, surfaceDepth) - 8`. Over the 15 dimensions that
// is 901120 painted columns, 245 separating readings and 900875 controls —
// measured by running this mode over all fifteen and adding up what it
// prints, NOT by multiplying 65536 by 15, which five of the windows do not
// carry.
//
// It refutes a clamp AT 0 and nothing lower. Every separating column is at
// depth exactly -1 (the per-dimension depth histograms printed below show
// `-1` and nothing under it) and the sweep's lowest raw anywhere is
// -1.134416806, so no column at depth <= -2 has been seen. A clamp at -1 or
// below predicts the same edge as no clamp on every column here.
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
// `band` is the OTHER direction of that, because counting only what the old
// reading cannot explain rewards a boundary for reaching further down — a
// boundary at the world floor would score perfectly on it. It censuses the
// band the new reading opens and the old leaves shut, and in particular the
// column's own SURFACE when it falls inside that band, which is the one
// position where what vanilla does with a gated material is pinned.
//
// `bands` is the confound control: the lowest y each of `probes/surf`'s
// `aps`, `bandlands` and `steep` entries painted. Same world, same pinned
// psl; the two rules that do not read y paint to the world floor and `aps`
// stops at -8..-2, which is what separates "the condition went false" from
// "the surface pass stopped".
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/aps-boundary-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aps-boundary-analyze
//   build/aps-boundary-analyze probe  .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/probes/apsb/k_p0 0 .fixtures/1.21.11/probes/apsb/r_m0_5 -0.5
//   build/aps-boundary-analyze golden .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/regions/seed-42/overworld/r.0.0.mca
//   build/aps-boundary-analyze band  .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/regions/seed-42/overworld/r.0.0.mca
//   build/aps-boundary-analyze depth .fixtures/1.21.11/worldgen 0 \
//       .fixtures/1.21.11/regions/seed-0/overworld/r.0.0.mca
//   build/aps-boundary-analyze bands .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/probes/surf/aps .fixtures/1.21.11/probes/surf/bandlands \
//       .fixtures/1.21.11/probes/surf/steep
//   build/aps-boundary-analyze sweep .fixtures/1.21.11/worldgen \
//       -16384 16384 -16384 16384 12 \
//       0 1 -1 42 -4172144997902289642 2891948927356891 \
//       9223372036854775807 -9223372036854775808
//   build/aps-boundary-analyze window .fixtures/1.21.11/worldgen -4172144997902289642 \
//       .fixtures/1.21.11/regions/seed--4172144997902289642/overworld/r.4.3.mca \
//       2282 1879 2282 1880 2283 1880 2284 1880
//   build/aps-boundary-analyze clamp .fixtures/1.21.11/worldgen 42 \
//       .fixtures/1.21.11/probes/apsc_s2/k_p100/r.6.10.mca 100 \
//       .fixtures/1.21.11/probes/apsc_s2/k_m20/r.6.10.mca -20
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <utility>
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

    [[nodiscard]] double surfaceDepthRaw(std::int32_t x, std::int32_t z) const {
        return executor_.surfaceDepthRaw(x, z);
    }

    [[nodiscard]] std::int32_t preliminarySurface(std::int32_t x, std::int32_t z) const {
        return static_cast<std::int32_t>(std::floor(
            interpreter_.evaluate(overworld_.router.at(settings::RouterEntry::PreliminarySurfaceLevel),
                                  density::Point{.x = x, .y = 0, .z = z})));
    }

    /// The same value BEFORE the floor. The interpolation question below
    /// needs the raw number: two columns whose floors agree can still sit
    /// either side of a lattice edge.
    [[nodiscard]] double preliminarySurfaceRaw(std::int32_t x, std::int32_t z) const {
        return interpreter_.evaluate(
            overworld_.router.at(settings::RouterEntry::PreliminarySurfaceLevel),
            density::Point{.x = x, .y = 0, .z = z});
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

/// Anything a probe's surface rule paints: the `diamond_block` the marker
/// rules place, and the clay bands `bandlands` places. Everything else in a
/// probe column is the terrain filler's own stone.
[[nodiscard]] bool painted(const std::string& name) {
    return name == "minecraft:diamond_block" || name.find("terracotta") != std::string::npos;
}

/// `bands`: the lowest y a probe entry's surface rule painted, per column.
///
/// This is the control that separates "the condition went false" from "the
/// vanilla surface pass stopped": a pass that stopped at a height would clip
/// EVERY rule at that height, so a rule with no condition at all (`bandlands`)
/// and a rule whose condition does not read y (`steep`) must keep painting
/// below wherever `above_preliminary_surface` stops. Same world, same pinned
/// `preliminary_surface_level`, three entries of `probes/surf`.
void scoreBands(const World& world, const std::filesystem::path& dir) {
    const std::filesystem::path region = dir / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(region)) {
        std::fprintf(stderr, "missing %s\n", region.c_str());
        return;
    }
    const auto file = region::RegionFile::open(region);
    const std::int32_t minY = world.geometry().minY;
    const std::int32_t topY = minY + world.geometry().height;

    long long columns = 0;
    long long toFloor = 0;
    std::map<std::int32_t, long long> lowest;

    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (std::int32_t y = minY; y < topY; ++y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr || !painted(block->name)) {
                            continue;
                        }
                        ++columns;
                        ++lowest[y];
                        toFloor += static_cast<long long>(y == minY);
                        break;
                    }
                }
            }
        }
    }

    std::printf("%s  painted columns=%lld  reaching the world floor (%d)=%lld\n",
                dir.filename().c_str(), columns, minY, toFloor);
    std::printf("    lowest painted y:");
    for (const auto& [value, count] : lowest) {
        std::printf(" %d:%lld", value, count);
    }
    std::printf("\n");
}

/// The result states that appear inside `surface_rule.sequence[1]` — the
/// subtree `above_preliminary_surface` gates — and nowhere else in the
/// overworld's tree, less the three the terrain filler itself writes and so
/// which cannot be attributed to the subtree: `air`, `water` and `stone`.
///
/// A block from this list is one the gated subtree CAN place and the terrain
/// filler cannot — not one only that subtree can place. The list is derived
/// from the surface-rule TREE, which says nothing about FEATURES, and features
/// run after the surface pass: vanilla places gravel, sand, dirt, podzol,
/// coarse_dirt, snow_block and packed_ice from disks, ore blobs, trees, ice
/// patches and icebergs. Counting these therefore gives an UPPER BOUND on the
/// positions the gated subtree ran at, not a count of them.
[[nodiscard]] bool gatedOnlyMaterial(const std::string& name) {
    static const std::vector<std::string> kMaterials{
        "minecraft:calcite",     "minecraft:coarse_dirt",     "minecraft:dirt",
        "minecraft:grass_block", "minecraft:gravel",          "minecraft:ice",
        "minecraft:mud",         "minecraft:mycelium",        "minecraft:orange_terracotta",
        "minecraft:packed_ice",  "minecraft:podzol",          "minecraft:powder_snow",
        "minecraft:red_sand",    "minecraft:red_sandstone",   "minecraft:sand",
        "minecraft:sandstone",   "minecraft:snow_block",      "minecraft:terracotta",
        "minecraft:white_terracotta"};
    return std::find(kMaterials.begin(), kMaterials.end(), name) != kMaterials.end();
}

/// `band`: the OTHER direction of the golden re-scoring.
///
/// `golden` counts the blocks the OLD reading cannot explain. This counts the
/// positions the NEW reading turns the gated subtree ON at and the old one
/// left off — the band `[floor(psl) + depth - 8, floor(psl))` — and reports
/// what the server actually put in each of them. Two of those numbers are
/// worth separating:
///
///   * every band position, with its block. Most are buried stone, where the
///     subtree declines whatever the condition says, so the census alone
///     settles nothing.
///   * the band positions that are the column's SURFACE — the first solid
///     block reached from the sky, which is the position the surface pass
///     acts on. There the new reading says the materials subtree runs and the
///     old says it does not, so what the server left is the thing to look at.
///
/// Absence is still not refutation: the subtree is a `sequence` whose own
/// inner rules can decline at a position its gate opened. That is why this
/// reports a census rather than a verdict.
void scoreBand(const World& world, const std::filesystem::path& region) {
    const auto file = region::RegionFile::open(region);
    const std::int32_t minY = world.geometry().minY;
    const std::int32_t topY = minY + world.geometry().height;

    long long columns = 0;
    long long positions = 0;
    long long material = 0;
    long long surfaceInBand = 0;
    long long surfaceMaterial = 0;
    std::map<std::string, long long> blocks;
    std::map<std::string, long long> surfaceBlocks;

    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    const std::int32_t psl = world.preliminarySurface(x, z);
                    const std::int32_t depth = world.surfaceDepth(x, z);
                    ++columns;

                    // The column's own surface: the first block reached from
                    // the sky that is neither air nor fluid. That is the
                    // position the surface pass acts on, and the only one
                    // where "the subtree would have placed something" is a
                    // statement about vanilla rather than a guess.
                    std::int32_t surface = minY - 1;
                    for (std::int32_t y = topY - 1; y >= minY; --y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr) {
                            continue;
                        }
                        const std::string& name = block->name;
                        if (name == "minecraft:air" || name == "minecraft:cave_air" ||
                            name == "minecraft:water" || name == "minecraft:lava") {
                            continue;
                        }
                        surface = y;
                        break;
                    }

                    for (std::int32_t y = psl + depth - 8; y < psl; ++y) {
                        if (y < minY || y >= topY) {
                            continue;
                        }
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        const std::string name =
                            block == nullptr ? std::string("minecraft:air") : block->name;
                        ++positions;
                        ++blocks[name];
                        material += static_cast<long long>(gatedOnlyMaterial(name));
                        if (y != surface) {
                            continue;
                        }
                        ++surfaceInBand;
                        ++surfaceBlocks[name];
                        surfaceMaterial += static_cast<long long>(gatedOnlyMaterial(name));
                    }
                }
            }
        }
    }

    std::printf("%s  columns=%lld  band positions=%lld  holding a gated-only material=%lld\n",
                region.c_str(), columns, positions, material);
    std::printf("    the column's own surface inside the band: %lld, of them a gated-only "
                "material: %lld\n",
                surfaceInBand, surfaceMaterial);
    std::printf("    surface block there:");
    for (const auto& [name, count] : surfaceBlocks) {
        std::printf(" %s:%lld", name.c_str(), count);
    }
    std::printf("\n    every band position:");
    for (const auto& [name, count] : blocks) {
        std::printf(" %s:%lld", name.c_str(), count);
    }
    std::printf("\n");
}

/// `depth`: the census that decides whether a real region can separate
/// `8 - surfaceDepth` from `8 - max(0, surfaceDepth)` at all.
///
/// The two candidates coincide on every column whose surface depth is >= 0,
/// so the only evidence is a NEGATIVE-depth column, and in one of those they
/// part by exactly |depth| blocks: the window
///
///     [ floor(psl) + depth - 8,  floor(psl) - 9 ]
///
/// is where `8 - surfaceDepth` says the gated subtree runs and
/// `8 - max(0, surfaceDepth)` says it does not. A block only that subtree can
/// place, inside that window, refutes the clamp. Nothing else in the region
/// speaks to the difference — which is the measurement, not a caveat.
///
/// `psl` is read per column here, and in a real overworld it reaches the
/// condition sampled and interpolated instead (SPEC §11), so the window's
/// own position carries that open question's error. A column whose RAW psl is
/// constant over its neighbourhood is immune to it whatever the lattice's
/// pitch, and is counted separately for that reason.
void censusDepth(const World& world, const std::filesystem::path& region) {
    const auto file = region::RegionFile::open(region);
    const std::int32_t minY = world.geometry().minY;
    const std::int32_t topY = minY + world.geometry().height;

    long long columns = 0;
    long long negative = 0;
    long long rawNegative = 0;
    long long flatNegative = 0;
    long long windowMaterial = 0;
    long long windowGrass = 0;
    double lowestRaw = 1e30;
    std::map<std::int32_t, long long> depths;
    std::map<std::string, long long> windowBlocks;

    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    const double raw = world.surfaceDepthRaw(x, z);
                    const std::int32_t depth = world.surfaceDepth(x, z);
                    ++columns;
                    ++depths[depth];
                    lowestRaw = std::min(lowestRaw, raw);
                    rawNegative += static_cast<long long>(raw < 0.0);
                    if (depth >= 0) {
                        continue;
                    }
                    ++negative;

                    const std::int32_t psl = world.preliminarySurface(x, z);
                    const double rawPsl = world.preliminarySurfaceRaw(x, z);
                    bool flat = true;
                    for (const std::int32_t step : {-16, -8, -4, 4, 8, 16}) {
                        flat = flat && world.preliminarySurfaceRaw(x + step, z) == rawPsl &&
                               world.preliminarySurfaceRaw(x, z + step) == rawPsl;
                    }
                    flatNegative += static_cast<long long>(flat);

                    bool material = false;
                    bool grass = false;
                    std::string window;
                    for (std::int32_t y = psl + depth - 8; y <= psl - 9; ++y) {
                        if (y < minY || y >= topY) {
                            continue;
                        }
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        const std::string name = block == nullptr ? "<none>" : block->name;
                        ++windowBlocks[name];
                        window += " " + name;
                        material = material || gatedOnlyMaterial(name);
                        grass = grass || name == "minecraft:grass_block";
                    }
                    windowMaterial += static_cast<long long>(material);
                    windowGrass += static_cast<long long>(grass);
                    const auto top = chunk.highestNonAir(localX, localZ);
                    std::printf("    (%d,%d) depth=%d psl=%d flat=%d top=%d window[%d..%d]:%s\n", x,
                                z, depth, psl, static_cast<int>(flat), top.value_or(minY - 1),
                                psl + depth - 8, psl - 9, window.c_str());
                }
            }
        }
    }

    std::printf("%s  columns=%lld  depth<0: %lld", region.c_str(), columns, negative);
    if (negative > 0) {
        std::printf(" (1 in %lld)", columns / negative);
    }
    std::printf("  raw<0: %lld  lowest raw: %.6f  flat-psl among depth<0: %lld  "
                "window holds a gated-only material: %lld  grass_block: %lld\n",
                rawNegative, lowestRaw, flatNegative, windowMaterial, windowGrass);
    std::printf("    depth:");
    for (const auto& [value, count] : depths) {
        std::printf(" %d:%lld", value, count);
    }
    std::printf("\n    window blocks:");
    for (const auto& [name, count] : windowBlocks) {
        std::printf(" %s:%lld", name.c_str(), count);
    }
    std::printf("\n");
}

/// The biome the server recorded at a block position, read out of the
/// section's own 4x4x4 biome array. The rule tree branches on the biome at
/// five of its six top-level arms, so which arm could have run at a position
/// is not answerable without it.
[[nodiscard]] std::string biomeAt(const chunk::Chunk& chunk, int localX, std::int32_t y,
                                  int localZ) {
    const std::int32_t section = javamath::floorDiv(y, 16);
    for (const auto& piece : chunk.sections()) {
        if (piece.y != section || piece.biomePalette.empty()) {
            continue;
        }
        if (piece.biomes.empty()) {
            return piece.biomePalette.front();
        }
        const int quartX = localX / 4;
        const int quartZ = localZ / 4;
        const int quartY = javamath::floorMod(y, 16) / 4;
        const auto index = static_cast<std::size_t>((quartY * 16) + (quartZ * 4) + quartX);
        if (index >= piece.biomes.size()) {
            return "<out of range>";
        }
        const auto entry = static_cast<std::size_t>(piece.biomes[index]);
        return entry < piece.biomePalette.size() ? piece.biomePalette[entry] : "<bad palette>";
    }
    return "<no section>";
}

/// Whether a block counts as part of a solid run for `stone_depth`. The
/// condition's own guard is that the run is reset by anything non-solid, so
/// what matters here is only "not air and not a fluid".
[[nodiscard]] bool solidForStoneDepth(const chunk::BlockState* block) {
    if (block == nullptr) {
        return false;
    }
    return block->name != "minecraft:air" && block->name != "minecraft:cave_air" &&
           block->name != "minecraft:void_air" && block->name != "minecraft:water" &&
           block->name != "minecraft:lava";
}

/// `window`: the read that the whole clamp question comes down to.
///
/// At a column whose returned surface depth is -1 the two candidate
/// boundaries differ over exactly ONE block. `y >= psl + surfaceDepth - 8`
/// opens the gated surface-materials subtree at `y = psl - 9`;
/// `y >= psl + max(0, surfaceDepth) - 8` opens it only from `y = psl - 8`.
/// Everything at or above `psl - 8` is a control: both readings agree there.
///
/// WHAT THE TREE DOES AT DEPTH -1, which is what makes the read mean
/// something or not, worked out from .fixtures/1.21.11/worldgen before
/// looking at any block. The overworld's gated subtree is a `sequence` of
/// five arms, and at `surfaceDepth == -1`:
///
///   * arms 0, 2 and 4, and arm 3's first sub-arm, are gated by
///     `stone_depth(floor, offset 0, add_surface_depth false)`. That fires
///     only where the solid run's own depth is 0 — the TOP block of a run.
///   * arm 3's second sub-arm — the whole grass/dirt/gravel/mud family — is
///     gated by `stone_depth(floor, offset 0, add_surface_depth TRUE)`, whose
///     threshold is `0 + surfaceDepth = -1`. A run depth is never negative,
///     so this arm is OFF at every position of such a column, whatever the
///     boundary does.
///   * arm 3's third and fourth sub-arms add a `secondary_depth_range` of 6
///     and 30, so their thresholds reach 5 and 29 — they CAN fire buried, but
///     only in `warm_ocean`/`beach`/`snowy_beach` and `desert` respectively,
///     and they place sandstone.
///   * arm 1 is the badlands family only.
///   * `hole` is exactly `depth <= 0`, so it is TRUE here; `NOT(hole)` is
///     therefore false and the arm behind it is off.
///
/// So the prediction is sharp and it is mostly a NEGATIVE one. At `y = psl-9`
/// the unclamped reading places a distinguishable block only if that y is
/// either the top of a solid run (arms 0/2/4 reachable — gravel, sand,
/// sandstone, water, air) or buried in one of those four sand biomes. If
/// `y = psl-9` is ordinary buried stone in an ordinary biome, BOTH candidates
/// place nothing and the block is the terrain filler's, so the read cannot
/// separate them. That is a property of the column, not of the method, and
/// this mode prints the run depth and the biome so it is checkable rather
/// than asserted.
void readWindow(const World& world, const std::filesystem::path& region,
                const std::vector<std::pair<std::int32_t, std::int32_t>>& columns) {
    const auto file = region::RegionFile::open(region);
    std::printf("%s\n", region.c_str());

    for (const auto& [x, z] : columns) {
        const std::int32_t chunkX = javamath::floorDiv(x, 16);
        const std::int32_t chunkZ = javamath::floorDiv(z, 16);
        const std::int32_t localChunkX = javamath::floorMod(chunkX, 32);
        const std::int32_t localChunkZ = javamath::floorMod(chunkZ, 32);
        if (!file.hasChunk(localChunkX, localChunkZ)) {
            std::printf("  (%d,%d): chunk (%d,%d) is not in this region file\n", x, z, chunkX,
                        chunkZ);
            continue;
        }
        const auto chunk =
            chunk::Chunk::decode(nbt::read(file.readChunk(localChunkX, localChunkZ)).root);
        const int localX = javamath::floorMod(x, 16);
        const int localZ = javamath::floorMod(z, 16);

        const double raw = world.surfaceDepthRaw(x, z);
        const std::int32_t depth = world.surfaceDepth(x, z);
        const std::int32_t psl = world.preliminarySurface(x, z);
        const std::int32_t unclamped = psl + depth - 8;
        const std::int32_t clamped = psl + std::max(0, depth) - 8;
        const auto top = chunk.highestNonAir(localX, localZ);

        std::printf("  (%d,%d) status=%s depth=%d raw=%.9f psl=%d top=%d\n"
                    "    unclamped opens at y=%d, clamped at y=%d; they differ over [%d, %d]\n",
                    x, z, chunk.status().c_str(), depth, raw, psl, top.value_or(-999), unclamped,
                    clamped, unclamped, clamped - 1);

        // The solid run above each y, computed from the server's own blocks
        // rather than from this engine's terrain, since it is what the
        // server's surface pass would have seen.
        const std::int32_t lowest = unclamped - 4;
        const std::int32_t highest = std::max(clamped + 4, top.value_or(clamped) + 1);
        std::int32_t run = 0;
        std::map<std::int32_t, std::int32_t> runAt;
        for (std::int32_t y = highest; y >= lowest; --y) {
            run = solidForStoneDepth(chunk.blockAt(localX, y, localZ)) ? run + 1 : 0;
            runAt[y] = run;
        }

        for (std::int32_t y = highest; y >= lowest; --y) {
            const auto* block = chunk.blockAt(localX, y, localZ);
            const char* marker = "   ";
            if (y == unclamped) {
                marker = ">> ";
            } else if (y == clamped) {
                marker = " = ";
            }
            std::printf("    %sy=%4d %-34s run=%d floorDepth=%d %s\n", marker, y,
                        block == nullptr ? "<none>" : block->name.c_str(), runAt[y],
                        runAt[y] - 1, biomeAt(chunk, localX, y, localZ).c_str());
        }

        const auto* discriminating = chunk.blockAt(localX, unclamped, localZ);
        const auto* control = chunk.blockAt(localX, clamped, localZ);
        std::printf("    DISCRIMINATING y=%d: %s (run depth %d, biome %s)\n", unclamped,
                    discriminating == nullptr ? "<none>" : discriminating->name.c_str(),
                    runAt[unclamped] - 1, biomeAt(chunk, localX, unclamped, localZ).c_str());
        std::printf("    CONTROL        y=%d: %s (run depth %d)\n", clamped,
                    control == nullptr ? "<none>" : control->name.c_str(), runAt[clamped] - 1);
    }
}

/// `clamp`: the mode that answers the bottom-clamp question directly, on
/// `aps-clamp-probe.sh`'s output.
///
/// Each of that probe's dimensions is solid throughout, pins
/// `preliminary_surface_level` to a constant, and carries the single surface
/// rule `{ above_preliminary_surface -> diamond_block }`. So a column's
/// LOWEST marker is the condition's boundary at single-block resolution, with
/// no gated subtree, no `hole`, no `stone_depth` and no biome between the
/// condition and the readout — which is exactly what reading the overworld
/// could not give.
///
/// Two numbers, and they are not the same question:
///
///   * the WHOLE window, every painted column, scored against both
///     candidates. Every column whose depth is >= 0 agrees with both, so this
///     is the denominator and the sanity check rather than the answer: it
///     says the measured boundary still holds here, at this psl, off the
///     world origin.
///   * the columns whose depth is NEGATIVE, one line each. Those are the only
///     columns in existence that separate the two, and there the two
///     candidates predict lower edges one block apart.
void scoreClamp(const World& world, const std::filesystem::path& region, double psl) {
    const auto file = region::RegionFile::open(region);
    const auto floored = static_cast<std::int32_t>(std::floor(psl));

    long long columns = 0;
    long long broken = 0;
    long long unclampedRight = 0;
    long long clampedRight = 0;
    long long flooredRight = 0;
    long long nearZeroNegative = 0;
    long long negativeColumns = 0;
    long long negativeUnclamped = 0;
    long long negativeClamped = 0;
    long long negativeFloored = 0;
    std::map<std::int32_t, long long> depths;

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
                    // A region file holds 32x32 chunks; the probe forceloads
                    // only 8x8 of them, and the chunk's own coordinates are
                    // what say which columns these are.
                    const std::int32_t x = (chunk.x() * 16) + localX;
                    const std::int32_t z = (chunk.z() * 16) + localZ;
                    const double raw = world.surfaceDepthRaw(x, z);
                    const std::int32_t depth = world.surfaceDepth(x, z);
                    const auto flooredDepth = static_cast<std::int32_t>(std::floor(raw));
                    ++depths[depth];
                    const bool unclamped = lowest == floored + depth - 8;
                    const bool clamped = lowest == floored + std::max(0, depth) - 8;
                    // `floor` on the DEPTH rather than on psl. It parts from
                    // `(int)` on two disjoint sets and nowhere else: a raw at
                    // or below -1, and a raw in (-1, 0).
                    const bool flooredFits = lowest == floored + flooredDepth - 8;
                    unclampedRight += static_cast<long long>(unclamped);
                    clampedRight += static_cast<long long>(clamped);
                    flooredRight += static_cast<long long>(flooredFits);
                    nearZeroNegative += static_cast<long long>(raw < 0.0 && raw > -1.0);
                    if (depth >= 0) {
                        continue;
                    }
                    ++negativeColumns;
                    negativeUnclamped += static_cast<long long>(unclamped);
                    negativeClamped += static_cast<long long>(clamped);
                    negativeFloored += static_cast<long long>(flooredFits);
                    std::printf("    SEPARATING (%d,%d) depth=%d raw=%.9f lowest marker y=%d; "
                                "unclamped predicts %d, clamped %d, floor %d -> %s\n",
                                x, z, depth, raw, lowest, floored + depth - 8,
                                floored + std::max(0, depth) - 8, floored + flooredDepth - 8,
                                unclamped == clamped ? "both (no separation)"
                                : unclamped         ? "UNCLAMPED"
                                : clamped           ? "CLAMPED"
                                                    : "NEITHER");
                }
            }
        }
    }

    std::printf("%s psl=%g  painted columns=%lld  broken bands=%lld\n"
                "    psl + depth - 8            %lld of %lld\n"
                "    psl + max(0, depth) - 8    %lld of %lld\n"
                "    psl + floor(raw) - 8       %lld of %lld\n"
                "    columns with depth < 0: %lld (unclamped %lld, clamped %lld, floor %lld); "
                "raw in (-1, 0): %lld\n",
                region.parent_path().filename().c_str(), psl, columns, broken, unclampedRight,
                columns, clampedRight, columns, flooredRight, columns, negativeColumns,
                negativeUnclamped, negativeClamped, negativeFloored, nearZeroNegative);
    std::printf("    depth:");
    for (const auto& [value, count] : depths) {
        std::printf(" %d:%lld", value, count);
    }
    std::printf("\n");
}

/// One column the sweep below found at a negative depth.
struct Negative {
    std::int64_t seed;
    std::int32_t x;
    std::int32_t z;
    double raw;
};

/// `sweep`: the search that says how OFTEN the returned depth goes negative,
/// with its denominator, rather than whether one example exists.
///
/// The clamp question lives entirely in the depth's lower tail, and the tail
/// is thin: `surfaceDepthRaw` is `2.75 * surface + 3 + 0.25 * u`, so a
/// returned depth of -1 needs a raw at or below -1, i.e. the noise below
/// about -1.4545. Nothing about a region file is involved — this is pure
/// field evaluation — so the extent is bounded by CPU rather than by disk,
/// and it is reported as a RATE with its denominator, its window and its
/// seeds, because a count without those is what put two wrong universal
/// claims into this project's history.
///
/// Every column at a negative depth is printed. The tail HISTOGRAM is printed
/// too, in 0.01-wide bands from -1.10 up to -0.80: a bare count of the
/// sub--1.0 bin is one number with no shape, and the bands either side say
/// whether the tail is falling smoothly into the region that matters or the
/// hits are an artefact.
///
/// Threads get disjoint z strips of one seed at a time, each with its OWN
/// `World`: `Executor` is documented immutable and shareable, but a private
/// copy per thread costs a few seconds in total and removes the question.
void sweepSeeds(const std::filesystem::path& tree, const std::vector<std::int64_t>& seeds,
                std::int32_t x0, std::int32_t x1, std::int32_t z0, std::int32_t z1,
                unsigned threadCount) {
    // Bands of 0.01 over [-1.10, -0.80). The clamp makes bin 0 everything
    // below -1.09, i.e. it absorbs the whole far tail rather than dropping
    // it; the printed label is that band's lower edge.
    constexpr int kBins = 31;
    constexpr double kBinLow = -1.10;

    const auto columnsPerSeed =
        static_cast<long long>(x1 - x0) * static_cast<long long>(z1 - z0);
    std::printf("sweep: %zu seed(s), x in [%d, %d), z in [%d, %d) = %lld columns each, "
                "%lld in all, %u threads\n",
                seeds.size(), x0, x1, z0, z1, columnsPerSeed,
                columnsPerSeed * static_cast<long long>(seeds.size()), threadCount);

    std::vector<Negative> found;
    std::array<long long, kBins> histogram{};
    long long sweptTotal = 0;

    for (const std::int64_t seed : seeds) {
        std::vector<std::vector<Negative>> perThread(threadCount);
        std::vector<std::array<long long, kBins>> perThreadBins(threadCount);
        std::vector<double> perThreadLowest(threadCount, 1e30);
        std::vector<long long> perThreadColumns(threadCount, 0);
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        for (unsigned slot = 0; slot < threadCount; ++slot) {
            workers.emplace_back([&, slot] {
                const World local{tree, seed};
                auto& mine = perThread[slot];
                auto& bins = perThreadBins[slot];
                bins.fill(0);
                double lowest = 1e30;
                long long columns = 0;
                for (std::int32_t z = z0 + static_cast<std::int32_t>(slot); z < z1;
                     z += static_cast<std::int32_t>(threadCount)) {
                    for (std::int32_t x = x0; x < x1; ++x) {
                        const double raw = local.surfaceDepthRaw(x, z);
                        ++columns;
                        lowest = std::min(lowest, raw);
                        if (raw < -0.80) {
                            const auto bin = static_cast<int>(
                                std::floor((raw - kBinLow) / 0.01));
                            ++bins[static_cast<std::size_t>(std::clamp(bin, 0, kBins - 1))];
                        }
                        if (static_cast<std::int32_t>(raw) < 0) {
                            mine.push_back(Negative{.seed = seed, .x = x, .z = z, .raw = raw});
                        }
                    }
                }
                perThreadLowest[slot] = lowest;
                perThreadColumns[slot] = columns;
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        std::vector<Negative> mine;
        double lowest = 1e30;
        long long columns = 0;
        for (unsigned slot = 0; slot < threadCount; ++slot) {
            mine.insert(mine.end(), perThread[slot].begin(), perThread[slot].end());
            for (int bin = 0; bin < kBins; ++bin) {
                histogram[static_cast<std::size_t>(bin)] +=
                    perThreadBins[slot][static_cast<std::size_t>(bin)];
            }
            lowest = std::min(lowest, perThreadLowest[slot]);
            columns += perThreadColumns[slot];
        }
        std::sort(mine.begin(), mine.end(), [](const Negative& left, const Negative& right) {
            return std::pair{left.z, left.x} < std::pair{right.z, right.x};
        });
        sweptTotal += columns;
        std::printf("  seed %lld: %lld columns, lowest raw %.9f, %zu at depth < 0\n",
                    static_cast<long long>(seed), columns, lowest, mine.size());
        for (const auto& column : mine) {
            std::printf("    (%d,%d) raw %.9f depth %d chunk (%d,%d) region r.%d.%d\n", column.x,
                        column.z, column.raw, static_cast<std::int32_t>(column.raw),
                        javamath::floorDiv(column.x, 16), javamath::floorDiv(column.z, 16),
                        javamath::floorDiv(column.x, 512), javamath::floorDiv(column.z, 512));
        }
        found.insert(found.end(), mine.begin(), mine.end());
    }

    std::printf("total: %lld columns swept, %zu at depth < 0", sweptTotal, found.size());
    if (!found.empty()) {
        std::printf(" (1 in %lld)", sweptTotal / static_cast<long long>(found.size()));
    }
    std::printf("\n    raw tail:");
    for (int bin = 0; bin < kBins; ++bin) {
        std::printf(" [%.2f)%lld", kBinLow + (0.01 * bin),
                    histogram[static_cast<std::size_t>(bin)]);
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: aps-boundary-analyze probe  <worldgen> <seed> (<entry dir> <psl>)...\n"
                     "       aps-boundary-analyze golden <worldgen> <seed> <region.mca>...\n"
                     "       aps-boundary-analyze depth  <worldgen> <seed> <region.mca>...\n"
                     "       aps-boundary-analyze bands  <worldgen> <seed> <entry dir>...\n"
                     "       aps-boundary-analyze band   <worldgen> <seed> <region.mca>...\n"
                     "       aps-boundary-analyze window <worldgen> <seed> <region.mca> "
                     "(<x> <z>)...\n"
                     "       aps-boundary-analyze clamp  <worldgen> <seed> "
                     "(<region.mca> <psl>)...\n"
                     "       aps-boundary-analyze sweep  <worldgen> <x0> <x1> <z0> <z1> "
                     "<threads> <seed>...\n");
        return 2;
    }

    // `sweep` takes many seeds rather than one, so it is dispatched before the
    // shared single-seed World is built.
    if (std::string{argv[1]} == "sweep") {
        if (argc < 9) {
            std::fprintf(stderr, "sweep needs <worldgen> <x0> <x1> <z0> <z1> <threads> "
                                 "<seed>...\n");
            return 2;
        }
        std::vector<std::int64_t> seeds;
        for (int arg = 8; arg < argc; ++arg) {
            seeds.push_back(std::strtoll(argv[arg], nullptr, 10));
        }
        sweepSeeds(argv[2], seeds, static_cast<std::int32_t>(std::strtol(argv[3], nullptr, 10)),
                   static_cast<std::int32_t>(std::strtol(argv[4], nullptr, 10)),
                   static_cast<std::int32_t>(std::strtol(argv[5], nullptr, 10)),
                   static_cast<std::int32_t>(std::strtol(argv[6], nullptr, 10)),
                   static_cast<unsigned>(std::strtoul(argv[7], nullptr, 10)));
        return 0;
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
    if (mode == "depth") {
        for (int arg = 4; arg < argc; ++arg) {
            censusDepth(world, argv[arg]);
        }
        return 0;
    }
    if (mode == "band") {
        for (int arg = 4; arg < argc; ++arg) {
            scoreBand(world, argv[arg]);
        }
        return 0;
    }
    if (mode == "clamp") {
        for (int arg = 4; arg + 1 < argc; arg += 2) {
            scoreClamp(world, argv[arg], std::strtod(argv[arg + 1], nullptr));
        }
        return 0;
    }
    if (mode == "window") {
        if (argc < 7) {
            std::fprintf(stderr, "window needs <region.mca> and at least one <x> <z>\n");
            return 2;
        }
        std::vector<std::pair<std::int32_t, std::int32_t>> columns;
        for (int arg = 5; arg + 1 < argc; arg += 2) {
            columns.emplace_back(static_cast<std::int32_t>(std::strtol(argv[arg], nullptr, 10)),
                                 static_cast<std::int32_t>(std::strtol(argv[arg + 1], nullptr, 10)));
        }
        readWindow(world, argv[4], columns);
        return 0;
    }
    if (mode == "bands") {
        for (int arg = 4; arg < argc; ++arg) {
            scoreBands(world, argv[arg]);
        }
        return 0;
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
