// Stratum — where to point aquifer-presets-probe.sh, chosen from Stratum alone.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   stratum_aquifer_presets_scout screen  <worldgen-dir> <seed>...
//   stratum_aquifer_presets_scout confirm <worldgen-dir> <seed>
//                                         <amplified chunkX> <chunkZ>
//                                         <large_biomes chunkX> <chunkZ>
//                                         [amplified|overworld|large_biomes]
//   stratum_aquifer_presets_scout census  <worldgen-dir> <seed>
//
// WHY THIS EXISTS. The amplified and large_biomes presets reach aquifer
// regimes the default overworld's goldens barely touch: an aquifer above sea
// level under a preliminary surface (psl) above 141, the highest the level
// rule was fitted on (aquifer/lattice.hpp), and Q5.9's deep-dark override fed
// by large_biomes' own erosion and depth. A probe window chosen blind shows
// neither: on seven of the eight golden seeds amplified's psl never passes 88
// anywhere in r.0.0, and on seven large_biomes has no cell there that Q5.9
// dries above lambda. So the probe's seeds and windows are chosen here, from
// Stratum's own preset pipeline, before any server runs. That biases where
// the instrument looks, never what the server says.
//
// `screen` is cheap and ranks every 8x8-chunk window inside r.0.0 (the only
// region the conformance harness addresses, tests/support/fluid_flow.hpp):
//
//   * amplified — lattice points of the 16-block psl grid whose floored psl
//     is above 141, and (where there are any) cells centred at or above sea
//     level whose level is above it;
//   * large_biomes — aquifer cell centres above lambda where Q5.9's override
//     holds (read at the centre's own column) and whose level WITHOUT it is
//     wet: the cells the override can dry.
//
// `confirm` fills the chosen windows, every chunk, and prints the figures the
// conformance case's floors are set against (vanilla_aquifer_presets_test.cpp):
// the aquifer's footprint by kind (aquifers on against off, ore veins off —
// they never change a category), how much of it lies at or above sea level,
// how many distinct deciding cells read a psl gate above 141, how many
// above-sea blocks the local aquifer decides under such a cell, and for
// large_biomes how many blocks Q5.9 decides — the shipped filler against one
// whose `depth` router entry is aliased to `erosion`, which makes the
// override's `erosion < -0.225 && depth > 0.9` unsatisfiable while leaving
// every other read untouched.
//
// `census` fills every chunk of r.0.0 on amplified and ranks the windows by
// each of those figures — the screen's cell counts turned out to predict
// above-sea aquifer blocks poorly. A release build takes about four minutes
// a seed.
//
// Nothing it reads is committed: the worldgen tree is Mojang-derived
// (SPEC §12).
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace stratum;

namespace {

/// The window the probe forceloads, in chunks on a side, and the region it
/// must lie inside.
constexpr std::int32_t kWindow = 8;
constexpr std::int32_t kRegionChunks = 32;
constexpr std::int32_t kChunkBlocks = 16;
/// The highest psl the aquifer's level rule was fitted on (lattice.hpp).
constexpr std::int32_t kFittedPslMax = 141;

enum class Kind : std::uint8_t { Air, Water, Lava, Solid };

[[nodiscard]] Kind kindOf(const settings::BlockState& block) {
    const std::string name = block.name.toString();
    if (name == "minecraft:air" || name == "minecraft:cave_air") {
        return Kind::Air;
    }
    if (name == "minecraft:water") {
        return Kind::Water;
    }
    if (name == "minecraft:lava") {
        return Kind::Lava;
    }
    return Kind::Solid;
}

[[nodiscard]] bool isFluid(const Kind kind) {
    return kind == Kind::Water || kind == Kind::Lava;
}

/// The index of chunk (@p cx, @p cz) of r.0.0 in a row-major per-chunk table.
[[nodiscard]] std::size_t chunkIndex(const std::int32_t cx, const std::int32_t cz) {
    return (static_cast<std::size_t>(cz) * static_cast<std::size_t>(kRegionChunks)) +
           static_cast<std::size_t>(cx);
}

/// One preset's settings, graph and noises for one seed.
struct Preset {
    settings::LoadedSettings loaded;
    settings::NoiseSettings settings;
    density::NoiseRegistry noises;
    density::Interpreter interpreter;

    Preset(const data::Pack& pack, std::string_view id, const std::int64_t seed)
        : loaded(settings::loadAll(pack)),
          settings(loaded.settings.at(data::ResourceLocation::parse(std::string(id)))),
          noises(density::NoiseRegistry::create(pack, loaded.graph.referencedNoises(), seed,
                                                density::RandomSource::Xoroshiro)),
          interpreter(loaded.graph, noises,
                      density::CellGeometry{.width = settings.geometry.cellWidth(),
                                            .height = settings.geometry.cellHeight()}) {}

    // The interpreter holds references into this object's own members.
    Preset(const Preset&) = delete;
    Preset& operator=(const Preset&) = delete;
    Preset(Preset&&) = delete;
    Preset& operator=(Preset&&) = delete;
    ~Preset() = default;

    [[nodiscard]] double read(const settings::RouterEntry entry, const std::int32_t x,
                              const std::int32_t y, const std::int32_t z,
                              density::Interpreter::CornerCache& cache) const {
        return interpreter.evaluate(settings.router.at(entry),
                                    density::Point{.x = x, .y = y, .z = z}, cache);
    }
};

/// The best `count` window origins by `score`, highest first.
[[nodiscard]] std::vector<std::tuple<long long, std::int32_t, std::int32_t>>
bestWindows(const std::vector<long long>& perChunk, const std::size_t count) {
    std::vector<std::tuple<long long, std::int32_t, std::int32_t>> windows;
    for (std::int32_t oz = 0; oz + kWindow <= kRegionChunks; ++oz) {
        for (std::int32_t ox = 0; ox + kWindow <= kRegionChunks; ++ox) {
            long long sum = 0;
            for (std::int32_t cz = oz; cz < oz + kWindow; ++cz) {
                for (std::int32_t cx = ox; cx < ox + kWindow; ++cx) {
                    sum += perChunk[chunkIndex(cx, cz)];
                }
            }
            windows.emplace_back(sum, ox, oz);
        }
    }
    std::sort(windows.begin(), windows.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    windows.resize(std::min(count, windows.size()));
    return windows;
}

void screen(const data::Pack& pack, const std::int64_t seed) {
    const std::size_t chunks = static_cast<std::size_t>(kRegionChunks) * kRegionChunks;

    // Amplified: the psl lattice, every 16 blocks, credited to the chunk whose
    // minimum corner it is.
    const Preset amplified(pack, "minecraft:amplified", seed);
    density::Interpreter::CornerCache cache(amplified.interpreter.cacheSize());
    std::int32_t highestAmplified = 0;
    {
        std::vector<long long> perChunk(chunks, 0);
        std::int32_t& highest = highestAmplified;
        for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
                const std::int32_t psl = javamath::floorToInt(amplified.read(
                    settings::RouterEntry::PreliminarySurfaceLevel, cx * kChunkBlocks,
                    aquifer::kPreliminarySurfaceSampleY, cz * kChunkBlocks, cache));
                highest = std::max(highest, psl);
                perChunk[chunkIndex(cx, cz)] = psl > kFittedPslMax ? 1 : 0;
            }
        }
        std::printf("seed %lld amplified: highest psl %d; windows by psl points above %d:",
                    static_cast<long long>(seed), highest, kFittedPslMax);
        for (const auto& [score, ox, oz] : bestWindows(perChunk, 3)) {
            std::printf("  (%d, %d) %lld", ox, oz, score);
        }
        std::printf("\n");
    }
    if (highestAmplified > kFittedPslMax) {
        // The cells centred at or above sea level whose level is above it:
        // the only ones that can put aquifer fluid above the sea.
        const aquifer::CentreSource centres(seed, stratum::density::RandomSource::Xoroshiro);
        const std::int32_t sea = amplified.settings.seaLevel;
        const auto psl = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return amplified.read(settings::RouterEntry::PreliminarySurfaceLevel, x, y, z, cache);
        };
        std::vector<long long> wetAbove(chunks, 0);
        long long cells = 0;
        const std::int32_t lowestLayer = javamath::floorDiv(sea, aquifer::kCellPitchY);
        const std::int32_t highestLayer =
            javamath::floorDiv(amplified.settings.geometry.maxY(), aquifer::kCellPitchY);
        for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
                for (std::int32_t cy = lowestLayer; cy <= highestLayer; ++cy) {
                    const aquifer::CellIndex centre = centres.centreOf(cx, cy, cz);
                    if (centre.y < sea) {
                        continue;
                    }
                    const aquifer::SamplePos spreadPos = aquifer::spreadSample(
                        aquifer::CellIndex{.x = cx, .y = cy, .z = cz}, centre);
                    const aquifer::CellFluid cell{
                        .centreY = centre.y,
                        .surface = aquifer::readPreliminarySurface(psl, centre, sea),
                        .seaLevel = sea,
                        .floodedness = amplified.read(settings::RouterEntry::FluidLevelFloodedness,
                                                      centre.x, centre.y, centre.z, cache),
                        .spread = amplified.read(settings::RouterEntry::FluidLevelSpread,
                                                 spreadPos.x, spreadPos.y, spreadPos.z, cache),
                        .deepDark = aquifer::isDeepDark(
                            amplified.read(settings::RouterEntry::Erosion, centre.x, centre.y,
                                           centre.z, cache),
                            amplified.read(settings::RouterEntry::Depth, centre.x, centre.y,
                                           centre.z, cache))};
                    if (aquifer::cellLevel(cell).level <= sea) {
                        continue;
                    }
                    ++cells;
                    const std::int32_t chunkX = javamath::floorDiv(centre.x, kChunkBlocks);
                    const std::int32_t chunkZ = javamath::floorDiv(centre.z, kChunkBlocks);
                    if (chunkX >= 0 && chunkZ >= 0 && chunkX < kRegionChunks &&
                        chunkZ < kRegionChunks) {
                        ++wetAbove[chunkIndex(chunkX, chunkZ)];
                    }
                }
            }
        }
        std::printf("seed %lld amplified: %lld cells wet above sea level; windows by those:",
                    static_cast<long long>(seed), cells);
        for (const auto& [score, ox, oz] : bestWindows(wetAbove, 3)) {
            std::printf("  (%d, %d) %lld", ox, oz, score);
        }
        std::printf("\n");
    }

    // large_biomes: the cells Q5.9 can dry.
    {
        const Preset large(pack, "minecraft:large_biomes", seed);
        density::Interpreter::CornerCache largeCache(large.interpreter.cacheSize());
        const aquifer::CentreSource centres(seed, stratum::density::RandomSource::Xoroshiro);
        const std::int32_t sea = large.settings.seaLevel;
        const std::int32_t lambda = aquifer::lambdaLevel(sea);
        const auto at = [&](const settings::RouterEntry entry) {
            return [&large, &largeCache, entry](std::int32_t x, std::int32_t y, std::int32_t z) {
                return large.read(entry, x, y, z, largeCache);
            };
        };
        const auto psl = at(settings::RouterEntry::PreliminarySurfaceLevel);
        std::vector<long long> perChunk(chunks, 0);
        long long deepDark = 0;
        long long wet = 0;
        const std::int32_t lowestLayer = javamath::floorDiv(lambda, aquifer::kCellPitchY);
        const std::int32_t highestLayer = javamath::floorDiv(sea, aquifer::kCellPitchY) + 1;
        for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
                for (std::int32_t cy = lowestLayer; cy <= highestLayer; ++cy) {
                    const aquifer::CellIndex centre = centres.centreOf(cx, cy, cz);
                    if (centre.y <= lambda) {
                        continue;
                    }
                    const double erosion = large.read(settings::RouterEntry::Erosion, centre.x,
                                                      centre.y, centre.z, largeCache);
                    const double depth = large.read(settings::RouterEntry::Depth, centre.x,
                                                    centre.y, centre.z, largeCache);
                    if (!aquifer::isDeepDark(erosion, depth)) {
                        continue;
                    }
                    ++deepDark;
                    const aquifer::SamplePos spreadPos = aquifer::spreadSample(
                        aquifer::CellIndex{.x = cx, .y = cy, .z = cz}, centre);
                    const aquifer::CellFluid cell{
                        .centreY = centre.y,
                        .surface = aquifer::readPreliminarySurface(psl, centre, sea),
                        .seaLevel = sea,
                        .floodedness = large.read(settings::RouterEntry::FluidLevelFloodedness,
                                                  centre.x, centre.y, centre.z, largeCache),
                        .spread = large.read(settings::RouterEntry::FluidLevelSpread, spreadPos.x,
                                             spreadPos.y, spreadPos.z, largeCache),
                        .deepDark = false};
                    if (aquifer::cellLevel(cell).level > lambda) {
                        ++wet;
                        const std::int32_t chunkX = javamath::floorDiv(centre.x, kChunkBlocks);
                        const std::int32_t chunkZ = javamath::floorDiv(centre.z, kChunkBlocks);
                        if (chunkX >= 0 && chunkZ >= 0 && chunkX < kRegionChunks &&
                            chunkZ < kRegionChunks) {
                            ++perChunk[chunkIndex(chunkX, chunkZ)];
                        }
                    }
                }
            }
        }
        std::printf("seed %lld large_biomes: %lld deep-dark centres above lambda, %lld wet "
                    "without the override; windows by those:",
                    static_cast<long long>(seed), deepDark, wet);
        for (const auto& [score, ox, oz] : bestWindows(perChunk, 3)) {
            std::printf("  (%d, %d) %lld", ox, oz, score);
        }
        std::printf("\n");
    }
}

/// What the aquifer changed, aquifers on against off.
struct Footprint {
    long long total = 0;
    long long barrier = 0;   // fluid -> solid
    long long dry = 0;       // fluid -> air
    long long localLava = 0; // water -> lava
    long long aboveSea = 0;  // at or above sea level, air -> fluid or solid
    long long other = 0;
};

void tally(Footprint& footprint, const Kind off, const Kind on, const std::int32_t y,
           const std::int32_t sea) {
    if (off == on) {
        return;
    }
    ++footprint.total;
    if (y >= sea && off == Kind::Air) {
        ++footprint.aboveSea;
    } else if (isFluid(off) && on == Kind::Solid) {
        ++footprint.barrier;
    } else if (isFluid(off) && on == Kind::Air) {
        ++footprint.dry;
    } else if (off == Kind::Water && on == Kind::Lava) {
        ++footprint.localLava;
    } else {
        ++footprint.other;
    }
}

void add(Footprint& into, const Footprint& more) {
    into.total += more.total;
    into.barrier += more.barrier;
    into.dry += more.dry;
    into.localLava += more.localLava;
    into.aboveSea += more.aboveSea;
    into.other += more.other;
}

void print(const char* label, const Footprint& footprint) {
    std::printf("  %-13s footprint %lld: barrier %lld, dry %lld, local lava %lld, above sea "
                "%lld, other %lld\n",
                label, footprint.total, footprint.barrier, footprint.dry, footprint.localLava,
                footprint.aboveSea, footprint.other);
}

using Cell = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

/// One chunk's figures.
struct ChunkScore {
    Footprint footprint;
    long long deepDark = 0;
    /// Distinct nearest cells, gated above kFittedPslMax, of the blocks the
    /// aquifer changed.
    std::set<Cell> highGateCells;
    long long highGateBlocks = 0;
    /// Blocks at or above sea level the LOCAL aquifer decides — not solid
    /// by density, and at or below the chunk's y_skip — whether it decides
    /// air, fluid or barrier; and those whose nearest cell is gated above
    /// kFittedPslMax.
    long long aboveSeaConsulted = 0;
    long long aboveSeaConsultedHighGate = 0;
};

/// One preset's three first-pass fillers (ore veins off: they never change a
/// category): as shipped, without the aquifer, and without Q5.9.
class Scorer {
public:
    Scorer(const data::Pack& pack, const char* id, const std::int64_t seed)
        : preset_(pack, id, seed), shipped_(withoutVeins(preset_.settings)),
          off_(withoutAquifer(shipped_)), noDeepDark_(withoutDeepDark(shipped_)),
          centres_(seed, stratum::density::RandomSource::Xoroshiro),
          withAquifer_(
              terrain::ChunkFiller::compile(preset_.loaded.graph, preset_.noises, shipped_)),
          withoutAquifer_(
              terrain::ChunkFiller::compile(preset_.loaded.graph, preset_.noises, off_)),
          rival_(terrain::ChunkFiller::compile(preset_.loaded.graph, preset_.noises, noDeepDark_)) {
    }

    // The fillers hold pointers to this object's own settings.
    Scorer(const Scorer&) = delete;
    Scorer& operator=(const Scorer&) = delete;
    Scorer(Scorer&&) = delete;
    Scorer& operator=(Scorer&&) = delete;
    ~Scorer() = default;

    [[nodiscard]] ChunkScore score(const std::int32_t cx, const std::int32_t cz,
                                   const bool scoreDeepDark) const {
        const std::int32_t sea = shipped_.seaLevel;
        const auto& geometry = shipped_.geometry;
        terrain::ChunkBuffer on(geometry);
        withAquifer_.fill(cx, cz, on);
        terrain::ChunkBuffer without(geometry);
        withoutAquifer_.fill(cx, cz, without);
        std::optional<terrain::ChunkBuffer> rivalBuffer;
        if (scoreDeepDark) {
            rivalBuffer.emplace(geometry);
            rival_.fill(cx, cz, *rivalBuffer);
        }
        density::Interpreter::CornerCache cache(preset_.interpreter.cacheSize());
        const density::FlatCacheWindow window = terrain::ChunkFiller::flatCacheWindow(cx, cz);
        const density::NodeIndex pslNode =
            shipped_.router.at(settings::RouterEntry::PreliminarySurfaceLevel);
        const auto psl = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return preset_.interpreter.evaluate(pslNode, density::Point{.x = x, .y = y, .z = z},
                                                cache, window);
        };
        // One scan per centre per chunk: the filler's own StatusCache has the
        // same scope.
        std::map<Cell, std::int32_t> gates;
        // The chunk's y_skip, as ChunkFiller::fill computes it: above it the
        // global picker decides and the local aquifer is never consulted.
        const aquifer::YSkipRectangle rectangle =
            aquifer::ySkipRectangle(cx * kChunkBlocks, cz * kChunkBlocks);
        std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
        for (std::int32_t z = rectangle.minZ; z <= rectangle.maxZ;
             z += aquifer::kYSkipSampleStride) {
            for (std::int32_t x = rectangle.minX; x <= rectangle.maxX;
                 x += aquifer::kYSkipSampleStride) {
                maxSurface =
                    std::max(maxSurface,
                             javamath::floorToInt(psl(x, aquifer::kPreliminarySurfaceSampleY, z)));
            }
        }
        const std::int32_t ySkip = aquifer::ySkip(maxSurface);
        ChunkScore result;
        for (std::int32_t y = geometry.minY; y < geometry.maxY(); ++y) {
            for (int z = 0; z < kChunkBlocks; ++z) {
                for (int x = 0; x < kChunkBlocks; ++x) {
                    const Kind kindOn = kindOf(on.at(x, y, z));
                    const Kind kindOff = kindOf(without.at(x, y, z));
                    tally(result.footprint, kindOff, kindOn, y, sea);
                    if (rivalBuffer.has_value() && kindOf(rivalBuffer->at(x, y, z)) != kindOn) {
                        ++result.deepDark;
                    }
                    const bool consulted = kindOff != Kind::Solid && y <= ySkip;
                    if (!consulted) {
                        continue;
                    }
                    const aquifer::Source nearest =
                        aquifer::selectSources(centres_, (cx * kChunkBlocks) + x, y,
                                               (cz * kChunkBlocks) + z)
                            .nearest();
                    const Cell key{nearest.centre.x, nearest.centre.y, nearest.centre.z};
                    auto gate = gates.find(key);
                    if (gate == gates.end()) {
                        gate =
                            gates
                                .emplace(
                                    key,
                                    aquifer::readPreliminarySurface(psl, nearest.centre, sea).gate)
                                .first;
                    }
                    const bool highGate = gate->second > kFittedPslMax;
                    if (y >= sea) {
                        ++result.aboveSeaConsulted;
                        result.aboveSeaConsultedHighGate += highGate ? 1 : 0;
                    }
                    if (highGate && kindOn != kindOff) {
                        ++result.highGateBlocks;
                        result.highGateCells.insert(key);
                    }
                }
            }
        }
        return result;
    }

private:
    [[nodiscard]] static settings::NoiseSettings withoutVeins(settings::NoiseSettings value) {
        value.oreVeinsEnabled = false;
        return value;
    }

    [[nodiscard]] static settings::NoiseSettings withoutAquifer(settings::NoiseSettings value) {
        value.aquifersEnabled = false;
        return value;
    }

    /// Q5.9 unsatisfiable: depth read as erosion can never be both below
    /// -0.225 and above 0.9. Every other read is the shipped one.
    [[nodiscard]] static settings::NoiseSettings withoutDeepDark(settings::NoiseSettings value) {
        value.router.entries[static_cast<std::size_t>(settings::RouterEntry::Depth)] =
            value.router.at(settings::RouterEntry::Erosion);
        return value;
    }

    Preset preset_;
    settings::NoiseSettings shipped_;
    settings::NoiseSettings off_;
    settings::NoiseSettings noDeepDark_;
    aquifer::CentreSource centres_;
    terrain::ChunkFiller withAquifer_;
    terrain::ChunkFiller withoutAquifer_;
    terrain::ChunkFiller rival_;
};

void confirm(const data::Pack& pack, const std::int64_t seed,
             const std::pair<std::int32_t, std::int32_t> amplifiedOrigin,
             const std::pair<std::int32_t, std::int32_t> largeOrigin, const std::string_view only) {
    struct Arm {
        const char* label;
        const char* settings;
        std::pair<std::int32_t, std::int32_t> origin;
    };

    for (const Arm& arm : {Arm{"amplified", "minecraft:amplified", amplifiedOrigin},
                           Arm{"overworld", "minecraft:overworld", amplifiedOrigin},
                           Arm{"large_biomes", "minecraft:large_biomes", largeOrigin}}) {
        if (!only.empty() && only != arm.label) {
            continue;
        }
        const Scorer scorer(pack, arm.settings, seed);
        const bool scoreDeepDark = std::string_view(arm.label) == "large_biomes";
        Footprint footprint;
        long long deepDarkDecided = 0;
        std::set<Cell> highGateCells;
        long long highGateBlocks = 0;
        long long aboveSeaConsulted = 0;
        long long aboveSeaConsultedHighGate = 0;
        for (std::int32_t cz = arm.origin.second; cz < arm.origin.second + kWindow; ++cz) {
            for (std::int32_t cx = arm.origin.first; cx < arm.origin.first + kWindow; ++cx) {
                const ChunkScore chunk = scorer.score(cx, cz, scoreDeepDark);
                add(footprint, chunk.footprint);
                highGateCells.insert(chunk.highGateCells.begin(), chunk.highGateCells.end());
                highGateBlocks += chunk.highGateBlocks;
                deepDarkDecided += chunk.deepDark;
                aboveSeaConsulted += chunk.aboveSeaConsulted;
                aboveSeaConsultedHighGate += chunk.aboveSeaConsultedHighGate;
                if (chunk.deepDark > 0 || chunk.footprint.aboveSea > 0) {
                    std::printf("    %s chunk (%d, %d): above sea %lld, Q5.9 decides %lld\n",
                                arm.label, cx, cz, chunk.footprint.aboveSea, chunk.deepDark);
                }
            }
        }
        std::printf("seed %lld %s at chunk (%d, %d):\n", static_cast<long long>(seed), arm.label,
                    arm.origin.first, arm.origin.second);
        print(arm.label, footprint);
        std::printf("  %-13s deciding cells with psl gate above %d: %zu (%lld blocks)\n", arm.label,
                    kFittedPslMax, highGateCells.size(), highGateBlocks);
        std::printf("  %-13s above sea, the local aquifer decides %lld blocks, %lld under a "
                    "cell gated above %d\n",
                    arm.label, aboveSeaConsulted, aboveSeaConsultedHighGate, kFittedPslMax);
        if (scoreDeepDark) {
            std::printf("  %-13s Q5.9 decides %lld blocks\n", arm.label, deepDarkDecided);
        }
    }
}

/// Every chunk of r.0.0 on amplified, then the windows ranked by aquifer
/// blocks decided above sea level, with their deciding high-gate cells.
void census(const data::Pack& pack, const std::int64_t seed) {
    const Scorer scorer(pack, "minecraft:amplified", seed);
    std::vector<ChunkScore> chunks;
    for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
            chunks.push_back(scorer.score(cx, cz, false));
        }
    }
    const auto chunkAt = [&chunks](std::int32_t cx, std::int32_t cz) -> const ChunkScore& {
        return chunks[chunkIndex(cx, cz)];
    };

    struct Window {
        std::int32_t ox;
        std::int32_t oz;
        long long aboveSea;
        long long consultedHighGate;
        std::size_t cells;
    };

    std::vector<Window> windows;
    for (std::int32_t oz = 0; oz + kWindow <= kRegionChunks; ++oz) {
        for (std::int32_t ox = 0; ox + kWindow <= kRegionChunks; ++ox) {
            Window window{.ox = ox, .oz = oz, .aboveSea = 0, .consultedHighGate = 0, .cells = 0};
            std::set<Cell> cells;
            for (std::int32_t cz = oz; cz < oz + kWindow; ++cz) {
                for (std::int32_t cx = ox; cx < ox + kWindow; ++cx) {
                    const ChunkScore& chunk = chunkAt(cx, cz);
                    window.aboveSea += chunk.footprint.aboveSea;
                    window.consultedHighGate += chunk.aboveSeaConsultedHighGate;
                    cells.insert(chunk.highGateCells.begin(), chunk.highGateCells.end());
                }
            }
            window.cells = cells.size();
            windows.push_back(window);
        }
    }
    const auto report = [&](const char* by, auto key) {
        std::sort(windows.begin(), windows.end(),
                  [&key](const Window& a, const Window& b) { return key(a) > key(b); });
        std::printf("seed %lld amplified census, best windows by %s:\n",
                    static_cast<long long>(seed), by);
        for (std::size_t i = 0; i < 5 && i < windows.size(); ++i) {
            const Window& w = windows[i];
            std::printf("  (%d, %d) aquifer blocks above sea %lld; above-sea blocks decided under "
                        "a gate above %d %lld; deciding cells gated above it %zu\n",
                        w.ox, w.oz, w.aboveSea, kFittedPslMax, w.consultedHighGate, w.cells);
        }
    };
    report("aquifer blocks above sea", [](const Window& w) { return w.aboveSea; });
    report("above-sea blocks under a high gate",
           [](const Window& w) { return w.consultedHighGate; });
    report("deciding cells gated high",
           [](const Window& w) { return static_cast<long long>(w.cells); });
}

[[nodiscard]] std::int32_t chunkArgument(const std::string& text) {
    const std::int32_t value = std::stoi(text);
    if (value < 0 || value + kWindow > kRegionChunks) {
        throw std::out_of_range("window origin " + text +
                                " does not keep an 8-chunk window inside r.0.0");
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (args.size() >= 3 && args[0] == "screen") {
            const data::Pack pack = data::Pack::open(args[1]);
            for (std::size_t i = 2; i < args.size(); ++i) {
                screen(pack, std::stoll(args[i]));
                std::fflush(stdout);
            }
            return 0;
        }
        if (args.size() == 3 && args[0] == "census") {
            census(data::Pack::open(args[1]), std::stoll(args[2]));
            return 0;
        }
        if ((args.size() == 7 || args.size() == 8) && args[0] == "confirm") {
            const data::Pack pack = data::Pack::open(args[1]);
            confirm(pack, std::stoll(args[2]), {chunkArgument(args[3]), chunkArgument(args[4])},
                    {chunkArgument(args[5]), chunkArgument(args[6])},
                    args.size() == 8 ? std::string_view(args[7]) : std::string_view());
            return 0;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    std::fprintf(stderr, "usage: stratum_aquifer_presets_scout screen <worldgen-dir> <seed>...\n"
                         "       stratum_aquifer_presets_scout confirm <worldgen-dir> <seed> "
                         "<amplified cx> <cz> <large_biomes cx> <cz> [one arm]\n"
                         "       stratum_aquifer_presets_scout census <worldgen-dir> <seed>\n");
    return 2;
}
