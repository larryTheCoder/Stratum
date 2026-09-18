// Stratum — reads back tools/analysis/aps-boundary-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Five modes. `probe` is the measurement; the other four are what make it
// worth believing — the control that it is the condition rather than the
// surface pass, both directions of the golden re-scoring, and the census that
// says which of the remaining candidates any world can still separate.
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
// The last of those is reported and is NOT separated here, and `depth` is the
// mode that says exactly how far from separated it is rather than leaving it
// as "the probes happened not to contain one". The two candidates differ only
// where the RETURNED depth is negative, which needs a raw value at or below -1
// because the cast truncates toward zero. Over 2097152 columns — every column
// of all eight golden overworld regions — the returned depth is never
// negative, and the lowest raw value is -0.449658 (seed 0; the other seven
// never go below zero at all).
//
// That is a fact about those regions, not about vanilla, and an earlier
// version of this header drew the wrong conclusion from it. Sweeping the same
// eight seeds over x, z in [-4096, 4096) — 536870912 columns — reaches depth
// -1 on four of them, all at seed -4172144997902289642 (x 2282-2284,
// z 1879-1880, raw -1.0085 to -1.0484). Vanilla's own amplitudes get there;
// they get there about one column in 134 million. No data pack is required —
// what is required is the region those columns are in, r.4.3.mca, which the
// golden set does not include. Said with the numbers rather than papered over.
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: aps-boundary-analyze probe  <worldgen> <seed> (<entry dir> <psl>)...\n"
                     "       aps-boundary-analyze golden <worldgen> <seed> <region.mca>...\n"
                     "       aps-boundary-analyze depth  <worldgen> <seed> <region.mca>...\n"
                     "       aps-boundary-analyze bands  <worldgen> <seed> <entry dir>...\n"
                     "       aps-boundary-analyze band   <worldgen> <seed> <region.mca>...\n");
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
    if (mode == "bands") {
        for (int arg = 4; arg < argc; ++arg) {
            scoreBands(world, argv[arg]);
        }
        return 0;
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
