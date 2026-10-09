// Stratum — writes a real, loadable Java Edition world from this engine's
// own terrain.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Deliberately not a `stratum` subcommand: a one-off deliverable, not a
// feature this build claims to support end to end (there is no level.dat
// writer here — see the usage note below). Nothing builds it; compile it by
// hand against the built library, as the header of any other one-shot tool
// under tools/analysis says.
//
// WHAT THIS PRODUCES. Vanilla's own overworld settings, aquifers and ore
// veins ON exactly as the pack has them (the golden regions keep both on too:
// tools/fetch-vanilla rewrites only the biome files), the real multi-noise
// biome search, and the full 287-rule surface-rule tree — the same first and
// surface passes `golden_overworld_test.cpp` scores against the golden
// regions. Carvers, features and structures are not part of this engine.
// Every chunk is written at Status "minecraft:full" with `isLightOn: 0`
// (chunk::encode's own doc): the server relights on load rather than this
// build guessing at vanilla's light-storage convention. Each chunk's
// `PostProcessing` lists carry the aquifer's fluid updates exactly as the
// server writes its own (tests/conformance/vanilla_aquifer_fluid_update_test
// .cpp holds the encoder to the server's bytes), so aquifer water and lava
// flow once a chunk loads, as in a world the server generated.
//
// NOT a level.dat writer, and not even a region writer: this writes one
// `<cx>_<cz>.nbt` chunk per file into the given directory, so that several
// processes can each take a disjoint chunk range; assemble-regions.cpp turns
// those into `region/*.mca` in one pass. Drop the result into an existing
// world's `region/` directory (a fresh vanilla world bootstrapped once and
// stopped) rather than expecting a whole save from this.
//
//   generate-world <chunk-dir> <seed> <chunkMinX> <chunkMinZ> <chunkCountX> <chunkCountZ>
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/writer.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace stratum;

namespace {

constexpr int kChunkWidth = 16;
constexpr int kQuartWidth = 4;

/// Solid, fluid or air, the same three categories the filler itself chooses
/// between — needed here only to compute heightmaps, which is the one thing
/// chunk::encode() leaves to the caller (its own doc explains why).
enum class Category { Air, Fluid, Solid };

[[nodiscard]] Category categorize(const settings::BlockState& block,
                                  const settings::NoiseSettings& settings) {
    if (block.name.toString() == "minecraft:air") {
        return Category::Air;
    }
    if (block == settings.defaultFluid) {
        return Category::Fluid;
    }
    return Category::Solid;
}

/// The biome at one quart-resolution point, via the SAME climate search
/// ChunkFiller::applySurfaceRules runs internally (terrain_filler.cpp) — not
/// exposed by ChunkFiller itself, so this mirrors it against a second
/// Interpreter built the same way ChunkFiller's own is.
[[nodiscard]] data::ResourceLocation biomeAt(density::Interpreter& interpreter,
                                             density::Interpreter::CornerCache& cache,
                                             const settings::NoiseSettings& settings,
                                             const biome::ParameterList& parameters,
                                             std::int32_t qx, std::int32_t qy, std::int32_t qz) {
    // The cache is what makes this affordable at all: `interpolated` nodes
    // recompute their eight corners from scratch with no cache, which
    // terrain_filler.cpp's own comment measures at 87 times slower over a
    // whole cell — the same saving ChunkFiller's own fill() takes for
    // exactly this reason.
    const density::Point at{.x = qx * kQuartWidth, .y = qy * kQuartWidth, .z = qz * kQuartWidth};
    const biome::ClimateSample sample{
        .temperature = interpreter.evaluate(settings.router.at(settings::RouterEntry::Temperature),
                                            at, cache),
        .humidity = interpreter.evaluate(settings.router.at(settings::RouterEntry::Vegetation), at,
                                         cache),
        .continentalness = interpreter.evaluate(
            settings.router.at(settings::RouterEntry::Continents), at, cache),
        .erosion =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Erosion), at, cache),
        .depth = interpreter.evaluate(settings.router.at(settings::RouterEntry::Depth), at, cache),
        .weirdness =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Ridges), at, cache)};
    return parameters.find(sample);
}

/// One chunk's blocks and biomes, built from ChunkFiller::fill() plus the
/// biome grid, into chunk::encode()'s own input shape.
[[nodiscard]] chunk::ChunkData buildChunkData(const terrain::ChunkBuffer& buffer,
                                              const settings::NoiseSettings& settings,
                                              std::int32_t chunkX, std::int32_t chunkZ,
                                              density::Interpreter& biomeInterpreter,
                                              density::Interpreter::CornerCache& biomeCache,
                                              const biome::ParameterList& biomeParameters) {
    const settings::NoiseGeometry& geometry = settings.geometry;
    const std::int32_t minY = geometry.minY;
    const std::int32_t sectionCount = geometry.height / chunk::kSectionSize;
    const std::int32_t lowestSection = javamath::floorDiv(minY, chunk::kSectionSize);
    const std::int32_t baseX = chunkX * kChunkWidth;
    const std::int32_t baseZ = chunkZ * kChunkWidth;

    // Quart grid: vanilla's own biome storage resolution — 4x4 horizontal
    // cells per section, one per kQuartWidth vertical step. The CornerCache
    // above is what keeps resolving all of them affordable.
    const std::int32_t quartsPerSection = chunk::kSectionSize / kQuartWidth;
    const std::int32_t baseQuartX = javamath::floorDiv(baseX, kQuartWidth);
    const std::int32_t baseQuartZ = javamath::floorDiv(baseZ, kQuartWidth);

    chunk::ChunkData data;
    data.x = chunkX;
    data.z = chunkZ;
    data.dataVersion = 4671; // 1.21.11 (observed on this build's own fixtures)
    data.lowestSection = lowestSection;
    data.status = "minecraft:full";

    std::vector<std::optional<int>> worldSurface(256);
    std::vector<std::optional<int>> oceanFloor(256);
    std::vector<std::optional<int>> motionBlocking(256);

    for (std::int32_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
        chunk::Section section;
        section.y = lowestSection + sectionIndex;
        const std::int32_t sectionBaseY = section.y * chunk::kSectionSize;

        std::map<std::string, std::uint16_t> paletteIndex;
        section.blocks.assign(chunk::kBlocksPerSection, 0);
        for (int ly = 0; ly < chunk::kSectionSize; ++ly) {
            const std::int32_t y = sectionBaseY + ly;
            for (int lz = 0; lz < kChunkWidth; ++lz) {
                for (int lx = 0; lx < kChunkWidth; ++lx) {
                    const settings::BlockState& block = buffer.at(lx, y, lz);
                    const std::string key = block.name.toString();
                    auto found = paletteIndex.find(key);
                    if (found == paletteIndex.end()) {
                        found = paletteIndex.emplace(key, static_cast<std::uint16_t>(
                                                              section.palette.size())).first;
                        chunk::BlockState state;
                        state.name = key;
                        section.palette.push_back(state);
                    }
                    const std::size_t index = (static_cast<std::size_t>(ly) * 256U) +
                                              (static_cast<std::size_t>(lz) * 16U) +
                                              static_cast<std::size_t>(lx);
                    section.blocks[index] = found->second;
                }
            }
        }

        std::map<std::string, std::uint16_t> biomePaletteIndex;
        section.biomes.assign(chunk::kBiomesPerSection, 0);
        const std::int32_t sectionBaseQuartY = javamath::floorDiv(sectionBaseY, kQuartWidth);
        for (std::int32_t qy = 0; qy < quartsPerSection; ++qy) {
            for (std::int32_t qz = 0; qz < 4; ++qz) {
                for (std::int32_t qx = 0; qx < 4; ++qx) {
                    const data::ResourceLocation biomeId =
                        biomeAt(biomeInterpreter, biomeCache, settings, biomeParameters,
                               baseQuartX + qx, sectionBaseQuartY + qy, baseQuartZ + qz);
                    const std::string biomeKey = biomeId.toString();
                    auto found = biomePaletteIndex.find(biomeKey);
                    if (found == biomePaletteIndex.end()) {
                        found = biomePaletteIndex
                                    .emplace(biomeKey, static_cast<std::uint16_t>(
                                                          section.biomePalette.size()))
                                    .first;
                        section.biomePalette.push_back(biomeKey);
                    }
                    const std::size_t index = (static_cast<std::size_t>(qy) * 16U) +
                                              (static_cast<std::size_t>(qz) * 4U) +
                                              static_cast<std::size_t>(qx);
                    section.biomes[index] = found->second;
                }
            }
        }

        data.sections.push_back(std::move(section));
    }

    for (int lz = 0; lz < kChunkWidth; ++lz) {
        for (int lx = 0; lx < kChunkWidth; ++lx) {
            std::optional<int> surface;
            std::optional<int> floor;
            std::optional<int> blocking;
            for (std::int32_t y = minY + geometry.height - 1; y >= minY; --y) {
                const Category category = categorize(buffer.at(lx, y, lz), settings);
                if (category == Category::Air) {
                    continue;
                }
                if (!surface.has_value()) {
                    surface = y;
                }
                if (!blocking.has_value()) {
                    blocking = y;
                }
                if (category == Category::Solid && !floor.has_value()) {
                    floor = y;
                }
                if (surface.has_value() && floor.has_value() && blocking.has_value()) {
                    break;
                }
            }
            const std::size_t column = (static_cast<std::size_t>(lz) * 16U) +
                                       static_cast<std::size_t>(lx);
            worldSurface[column] = surface;
            oceanFloor[column] = floor;
            motionBlocking[column] = blocking;
        }
    }
    data.heightmaps.emplace_back(chunk::Heightmap::WorldSurface, worldSurface);
    data.heightmaps.emplace_back(chunk::Heightmap::OceanFloor, oceanFloor);
    data.heightmaps.emplace_back(chunk::Heightmap::MotionBlocking, motionBlocking);
    // No leaves exist in this build's output (no features/decorators), so
    // "ignoring leaves" changes nothing — the same array, written twice.
    data.heightmaps.emplace_back(chunk::Heightmap::MotionBlockingNoLeaves, motionBlocking);

    // The aquifer's fluid updates, which fill() leaves in the order the
    // server's own PostProcessing lists hold them: the server ticks each once
    // the chunk loads, so aquifer water and lava flow as they would in a
    // world it generated itself.
    for (const terrain::ChunkBuffer::FluidUpdate& update : buffer.fluidUpdates()) {
        data.postProcessing.push_back(chunk::PostProcessingMark{
            .localX = update.localX, .y = update.y, .localZ = update.localZ});
    }

    return data;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                     "usage: generate-world <region-dir> <seed> <chunkMinX> <chunkMinZ> "
                     "<chunkCountX> <chunkCountZ>\n");
        return 2;
    }
    const std::filesystem::path regionDir = argv[1];
    const auto seed = static_cast<std::int64_t>(std::atoll(argv[2]));
    const auto chunkMinX = static_cast<std::int32_t>(std::atoi(argv[3]));
    const auto chunkMinZ = static_cast<std::int32_t>(std::atoi(argv[4]));
    const auto chunkCountX = static_cast<std::int32_t>(std::atoi(argv[5]));
    const auto chunkCountZ = static_cast<std::int32_t>(std::atoi(argv[6]));

    const std::filesystem::path fixtures =
        std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
    const auto pack = data::Pack::open(fixtures / "worldgen");
    const auto loaded = settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(data::ResourceLocation::parse("minecraft:overworld"));

    std::ifstream parametersFile(fixtures / "biome_parameters" / "minecraft" / "overworld.json");
    std::stringstream parametersJson;
    parametersJson << parametersFile.rdbuf();
    const auto biomeParameters = biome::ParameterList::fromJson(
        nlohmann::json::parse(parametersJson.str()), data::ResourceLocation::parse("minecraft:overworld"));
    const auto biomeTemperatures = biome::TemperatureTable::fromPack(pack);
    const auto surfaceRules = surface::RuleGraph::resolve(
        overworld.surfaceRule, data::ResourceLocation::parse("minecraft:overworld"));

    auto wantedNoises = loaded.graph.referencedNoises();
    const auto surfaceNoises = surfaceRules.referencedNoises();
    wantedNoises.insert(wantedNoises.end(), surfaceNoises.begin(), surfaceNoises.end());
    wantedNoises.push_back(data::ResourceLocation::parse("minecraft:surface"));
    wantedNoises.push_back(data::ResourceLocation::parse("minecraft:surface_secondary"));
    wantedNoises.push_back(data::ResourceLocation::parse("minecraft:clay_bands_offset"));
    const auto noises = density::NoiseRegistry::create(pack, wantedNoises, seed,
                                                       density::RandomSource::Xoroshiro);

    const auto filler = terrain::ChunkFiller::compile(loaded.graph, noises, overworld,
                                                       &surfaceRules, &biomeParameters,
                                                       &biomeTemperatures);
    if (!filler.runsSurfaceRules()) {
        std::fprintf(stderr, "surface rules did not run:\n");
        for (const std::string& reason : filler.surfaceRulesBlockedBy()) {
            std::fprintf(stderr, "  %s\n", reason.c_str());
        }
        return 1;
    }
    density::Interpreter biomeInterpreter(
        loaded.graph, noises,
        density::CellGeometry{.width = overworld.geometry.cellWidth(),
                              .height = overworld.geometry.cellHeight()});

    // One raw, uncompressed chunk per file — NOT region files. Each
    // filename is unique by (cx, cz), so any number of instances of this
    // process can safely target the SAME output directory at once, each
    // given a disjoint chunk range: there is no shared file for them to
    // race on. assemble-regions.cpp does the actual region-file writing, in
    // one single-threaded pass over whatever this leaves behind — cheap,
    // since it does no terrain computation, only I/O and compression.
    std::filesystem::create_directories(regionDir);

    std::size_t done = 0;
    const std::size_t total =
        static_cast<std::size_t>(chunkCountX) * static_cast<std::size_t>(chunkCountZ);
    for (std::int32_t cz = chunkMinZ; cz < chunkMinZ + chunkCountZ; ++cz) {
        for (std::int32_t cx = chunkMinX; cx < chunkMinX + chunkCountX; ++cx) {
            terrain::ChunkBuffer buffer(overworld.geometry);
            filler.fill(cx, cz, buffer);
            density::Interpreter::CornerCache biomeCache(biomeInterpreter.cacheSize());
            const chunk::ChunkData data = buildChunkData(
                buffer, overworld, cx, cz, biomeInterpreter, biomeCache, biomeParameters);
            const auto bytes = nbt::write("", chunk::encode(data));

            const std::filesystem::path path =
                regionDir / (std::to_string(cx) + "_" + std::to_string(cz) + ".nbt");
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));

            ++done;
            if (done % 32 == 0 || done == total) {
                std::fprintf(stderr, "[%d,%d..%d,%d] %zu/%zu\n", chunkMinX, chunkMinZ, cx, cz, done,
                            total);
                std::fflush(stderr);
            }
        }
    }

    return 0;
}
