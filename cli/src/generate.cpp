// Stratum — `stratum generate`: region files from the library.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include "generate.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/writer.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stratum::cli {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::int32_t kSection = chunk::kSectionSize;
constexpr std::int32_t kQuartWidth = 4;
constexpr std::size_t kQuartsPerLayer = 16;
constexpr std::size_t kColumns = 256;

/// The DataVersion the pinned version's server writes into every chunk —
/// read off the golden regions, all of which carry it, not assumed. A
/// server reads it to decide whether a chunk needs upgrading, so it is the
/// pin's own number, never a newer one.
constexpr std::int32_t kDataVersion = 4671;

/// Empty sectors after every chunk, as tools/analysis/assemble-regions.cpp
/// leaves them: a chunk written without light grows the first time a server
/// loads it, and the slack lets it grow in place (`region::writeRegion`'s
/// own doc says why that matters).
constexpr std::size_t kLightGrowthPaddingSectors = 4;

/// Vanilla's world presets (`worldgen/world_preset/normal.json`,
/// `amplified.json`, `large_biomes.json`) pair these noise settings with
/// these biome sources, and their dimension types store these columns:
/// sections -4..19 in the overworld and 0..15 in the Nether and the End, as
/// every golden region's chunks hold them (minecraft.wiki's dimension type
/// page gives the same min_y and height).
constexpr auto kPresets = std::to_array<DimensionPreset>({
    {.name = "overworld",
     .noiseSettings = "minecraft:overworld",
     .biomeSource = "minecraft:overworld",
     .minY = -64,
     .height = 384},
    {.name = "amplified",
     .noiseSettings = "minecraft:amplified",
     .biomeSource = "minecraft:overworld",
     .minY = -64,
     .height = 384},
    {.name = "large_biomes",
     .noiseSettings = "minecraft:large_biomes",
     .biomeSource = "minecraft:overworld",
     .minY = -64,
     .height = 384},
    {.name = "nether",
     .noiseSettings = "minecraft:nether",
     .biomeSource = "minecraft:nether",
     .minY = 0,
     .height = 256},
    {.name = "end",
     .noiseSettings = "minecraft:end",
     .biomeSource = "minecraft:the_end",
     .minY = 0,
     .height = 256},
});

/// How a block counts for the heightmaps.
///
/// MEASURED, not assumed: over all eight golden overworld regions and all
/// eight End regions (4 194 304 columns), the server's stored WORLD_SURFACE
/// is the highest block that is not air; MOTION_BLOCKING and
/// MOTION_BLOCKING_NO_LEAVES the highest that is neither air nor powder
/// snow, fluids included; OCEAN_FLOOR the highest that is none of air,
/// fluid and powder snow — every column of all four, exactly. Counting
/// powder snow as blocking instead left 571 columns wrong, every one with
/// powder snow on top. The other blocks vanilla's surface rules can place
/// and those goldens do not hold (ice, mud, calcite, the terracottas, ...)
/// are taken to block motion, as every solid block measured did.
enum class HeightClass : std::uint8_t {
    Air,
    Fluid,
    /// Present, but motion passes through it.
    Passable,
    Blocking,
};

[[nodiscard]] HeightClass heightClassOf(const settings::BlockState& state) {
    const std::string name = state.name.toString();
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return HeightClass::Air;
    }
    if (name == "minecraft:water" || name == "minecraft:lava") {
        return HeightClass::Fluid;
    }
    if (name == "minecraft:powder_snow") {
        return HeightClass::Passable;
    }
    return HeightClass::Blocking;
}

[[nodiscard]] chunk::BlockState javaState(const settings::BlockState& state) {
    chunk::BlockState converted;
    converted.name = state.name.toString();
    // std::map iterates in key order: already the sorted order
    // chunk::BlockState compares by.
    for (const auto& [key, value] : state.properties) {
        converted.properties.emplace_back(key, value);
    }
    return converted;
}

/// One generation thread's scratch, reused from chunk to chunk.
struct Scratch {
    terrain::ChunkBuffer blocks;
    std::vector<const data::ResourceLocation*> biomes;
    StageTimes times;
};

/// Chunk (@p chunkX, @p chunkZ) as the NBT a server stores it as.
[[nodiscard]] std::vector<std::byte> generateChunk(const world::CompiledDimension& dimension,
                                                   const DimensionPreset& column,
                                                   std::int32_t chunkX, std::int32_t chunkZ,
                                                   Scratch& scratch) {
    const Clock::time_point start = Clock::now();
    dimension.fillBlocks(chunkX, chunkZ, scratch.blocks);
    const Clock::time_point filled = Clock::now();
    dimension.fillBiomes(chunkX, chunkZ, javamath::floorDiv(column.minY, kQuartWidth),
                         scratch.biomes);
    const Clock::time_point biomed = Clock::now();

    const terrain::ChunkBuffer& blocks = scratch.blocks;
    const settings::NoiseGeometry& geometry = dimension.geometry();
    const std::vector<settings::BlockState>& palette = blocks.palette();
    if (palette.empty() || heightClassOf(palette.front()) != HeightClass::Air) {
        // ChunkBuffer's own promise; the sections above the noise height
        // lean on it.
        throw GenerateError("the filled chunk's palette does not start with air");
    }
    std::vector<chunk::BlockState> states;
    std::vector<HeightClass> classes;
    states.reserve(palette.size());
    classes.reserve(palette.size());
    for (const settings::BlockState& state : palette) {
        states.push_back(javaState(state));
        classes.push_back(heightClassOf(state));
    }

    chunk::ChunkData data;
    data.x = chunkX;
    data.z = chunkZ;
    data.dataVersion = kDataVersion;
    data.lowestSection = javamath::floorDiv(column.minY, kSection);
    data.status = "minecraft:full";

    const std::int32_t sectionCount = column.height / kSection;
    data.sections.reserve(static_cast<std::size_t>(sectionCount));
    // Buffer palette index -> this section's, or -1 for "not used yet".
    std::vector<int> local(palette.size());
    for (std::int32_t i = 0; i < sectionCount; ++i) {
        chunk::Section section;
        section.y = data.lowestSection + i;
        const std::int32_t baseY = section.y * kSection;
        std::ranges::fill(local, -1);
        section.blocks.resize(chunk::kBlocksPerSection);
        // Sections are whole: the noise height was checked to be a run of
        // them, so a section is either all generated or all above or below.
        const bool generated = baseY >= geometry.minY && baseY < geometry.maxY();
        for (int y = 0; y < kSection; ++y) {
            for (int z = 0; z < kSection; ++z) {
                for (int x = 0; x < kSection; ++x) {
                    const std::uint16_t index =
                        generated ? blocks.paletteIndexAt(x, baseY + y, z) : std::uint16_t{0};
                    if (local[index] < 0) {
                        local[index] = static_cast<int>(section.palette.size());
                        section.palette.push_back(states[index]);
                    }
                    section
                        .blocks[(static_cast<std::size_t>(y) * 256U) +
                                (static_cast<std::size_t>(z) * 16U) + static_cast<std::size_t>(x)] =
                        static_cast<std::uint16_t>(local[index]);
                }
            }
        }

        // The section's 64 biome cells, in the order fillBiomes wrote them:
        // (qy * 4 + qz) * 4 + qx, qy counted up from the column's foot.
        const std::size_t firstCell =
            static_cast<std::size_t>((baseY - column.minY) / kQuartWidth) * kQuartsPerLayer;
        std::vector<const data::ResourceLocation*> seen;
        section.biomes.resize(chunk::kBiomesPerSection);
        for (std::size_t cell = 0; cell < chunk::kBiomesPerSection; ++cell) {
            const data::ResourceLocation* biome = scratch.biomes[firstCell + cell];
            const auto found = std::ranges::find(seen, biome);
            const auto at = static_cast<std::size_t>(found - seen.begin());
            if (found == seen.end()) {
                seen.push_back(biome);
                section.biomePalette.push_back(biome->toString());
            }
            section.biomes[cell] = static_cast<std::uint16_t>(at);
        }
        data.sections.push_back(std::move(section));
    }

    std::vector<std::optional<int>> worldSurface(kColumns);
    std::vector<std::optional<int>> motionBlocking(kColumns);
    std::vector<std::optional<int>> oceanFloor(kColumns);
    for (int z = 0; z < kSection; ++z) {
        for (int x = 0; x < kSection; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(z) * 16U) + static_cast<std::size_t>(x);
            // Above the noise height every block is air.
            for (std::int32_t y = geometry.maxY() - 1; y >= geometry.minY; --y) {
                const HeightClass kind = classes[blocks.paletteIndexAt(x, y, z)];
                if (kind == HeightClass::Air) {
                    continue;
                }
                if (!worldSurface[at].has_value()) {
                    worldSurface[at] = y;
                }
                if (kind == HeightClass::Passable) {
                    continue;
                }
                if (!motionBlocking[at].has_value()) {
                    motionBlocking[at] = y;
                }
                if (kind == HeightClass::Blocking) {
                    oceanFloor[at] = y;
                    break;
                }
            }
        }
    }
    data.heightmaps.emplace_back(chunk::Heightmap::WorldSurface, worldSurface);
    data.heightmaps.emplace_back(chunk::Heightmap::OceanFloor, oceanFloor);
    // Nothing this build places is a leaf, so "ignoring leaves" ignores
    // nothing.
    data.heightmaps.emplace_back(chunk::Heightmap::MotionBlocking, motionBlocking);
    data.heightmaps.emplace_back(chunk::Heightmap::MotionBlockingNoLeaves, motionBlocking);

    // The aquifer's fluid updates, in the order the server's own
    // PostProcessing lists hold them (ChunkBuffer::fluidUpdates).
    data.postProcessing.reserve(blocks.fluidUpdates().size());
    for (const terrain::ChunkBuffer::FluidUpdate& update : blocks.fluidUpdates()) {
        data.postProcessing.push_back(chunk::PostProcessingMark{
            .localX = update.localX, .y = update.y, .localZ = update.localZ});
    }

    std::vector<std::byte> bytes = nbt::write("", chunk::encode(data));
    const Clock::time_point encoded = Clock::now();
    scratch.times.fill += filled - start;
    scratch.times.biomes += biomed - filled;
    scratch.times.encode += encoded - biomed;
    return bytes;
}

[[nodiscard]] freeze::Pipeline loadPipeline(const std::filesystem::path& source) {
    if (std::filesystem::is_directory(source)) {
        if (!std::filesystem::is_directory(source / "worldgen")) {
            throw GenerateError(source.string() +
                                " is not a tools/fetch-vanilla version root: it has no "
                                "worldgen/ directory (or pass a frozen pipeline blob)");
        }
        return freeze::resolve(data::Pack::open(source / "worldgen"), source / "biome_parameters");
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw GenerateError("cannot open " + source.string() +
                            ": neither a version root nor a readable pipeline blob");
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> blob(static_cast<std::size_t>(std::max<std::streamoff>(size, 0)));
    in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (!in) {
        throw GenerateError("cannot read the pipeline blob " + source.string());
    }
    return freeze::read(blob);
}

[[nodiscard]] std::string describe(const DimensionPreset& preset) {
    return "'" + std::string(preset.name) + "' (noise settings " +
           std::string(preset.noiseSettings) + ", biome source " + std::string(preset.biomeSource) +
           ")";
}

[[nodiscard]] std::unique_ptr<world::CompiledDimension>
compile(freeze::Pipeline pipeline, const DimensionPreset& preset, std::int64_t seed) {
    std::unique_ptr<world::CompiledDimension> dimension;
    try {
        dimension = world::CompiledDimension::compile(
            std::move(pipeline), data::ResourceLocation::parse(preset.noiseSettings),
            world::BiomeSource::parse(preset.biomeSource), seed);
    } catch (const std::exception& error) {
        throw GenerateError("dimension " + describe(preset) +
                            " cannot be generated by this build: " + error.what());
    }
    const settings::NoiseGeometry& geometry = dimension->geometry();
    if (javamath::floorMod(geometry.minY, kSection) != 0 ||
        javamath::floorMod(geometry.height, kSection) != 0 || geometry.minY < preset.minY ||
        geometry.maxY() > preset.minY + preset.height) {
        throw GenerateError(
            "dimension " + describe(preset) + " generates y " + std::to_string(geometry.minY) +
            ".." + std::to_string(geometry.maxY() - 1) +
            ", which is not a run of whole sections inside the column it is "
            "stored in, y " +
            std::to_string(preset.minY) + ".." + std::to_string(preset.minY + preset.height - 1));
    }
    return dimension;
}

struct RegionPlan {
    std::int32_t regionX = 0;
    std::int32_t regionZ = 0;
    std::filesystem::path path;
    std::vector<std::pair<std::int32_t, std::int32_t>> chunks;
};

/// The regions @p range touches, z-major, each with its chunks in the range
/// in z-major order.
[[nodiscard]] std::vector<RegionPlan> planRegions(const ChunkRange& range,
                                                  const std::filesystem::path& out) {
    std::vector<RegionPlan> plans;
    const std::int32_t minRegionX = javamath::floorDiv(range.minX, region::kChunksPerAxis);
    const std::int32_t maxRegionX = javamath::floorDiv(range.maxX, region::kChunksPerAxis);
    const std::int32_t minRegionZ = javamath::floorDiv(range.minZ, region::kChunksPerAxis);
    const std::int32_t maxRegionZ = javamath::floorDiv(range.maxZ, region::kChunksPerAxis);
    for (std::int32_t regionZ = minRegionZ; regionZ <= maxRegionZ; ++regionZ) {
        for (std::int32_t regionX = minRegionX; regionX <= maxRegionX; ++regionX) {
            RegionPlan plan;
            plan.regionX = regionX;
            plan.regionZ = regionZ;
            plan.path =
                out / ("r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca");
            const std::int32_t firstX = std::max(range.minX, regionX * region::kChunksPerAxis);
            const std::int32_t lastX = std::min(range.maxX, (regionX * region::kChunksPerAxis) +
                                                                region::kChunksPerAxis - 1);
            const std::int32_t firstZ = std::max(range.minZ, regionZ * region::kChunksPerAxis);
            const std::int32_t lastZ = std::min(range.maxZ, (regionZ * region::kChunksPerAxis) +
                                                                region::kChunksPerAxis - 1);
            for (std::int32_t chunkZ = firstZ; chunkZ <= lastZ; ++chunkZ) {
                for (std::int32_t chunkX = firstX; chunkX <= lastX; ++chunkX) {
                    plan.chunks.emplace_back(chunkX, chunkZ);
                }
            }
            plans.push_back(std::move(plan));
        }
    }
    return plans;
}

void add(StageTimes& total, const StageTimes& part) {
    total.fill += part.fill;
    total.biomes += part.biomes;
    total.encode += part.encode;
    total.write += part.write;
}

/// Every chunk of @p plan, generated on @p threads threads; entry i is the
/// NBT of `plan.chunks[i]`, whichever thread made it.
[[nodiscard]] std::vector<std::vector<std::byte>>
generateRegion(const world::CompiledDimension& dimension, const DimensionPreset& column,
               const RegionPlan& plan, unsigned threads, StageTimes& times) {
    std::vector<std::vector<std::byte>> payloads(plan.chunks.size());
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex lock;
    std::exception_ptr firstError;

    const auto work = [&] {
        Scratch scratch{
            .blocks = terrain::ChunkBuffer(dimension.geometry()),
            .biomes = std::vector<const data::ResourceLocation*>(
                static_cast<std::size_t>(column.height / kQuartWidth) * kQuartsPerLayer),
            .times = {}};
        while (!failed.load()) {
            const std::size_t i = next.fetch_add(1);
            if (i >= plan.chunks.size()) {
                break;
            }
            const auto [chunkX, chunkZ] = plan.chunks[i];
            try {
                payloads[i] = generateChunk(dimension, column, chunkX, chunkZ, scratch);
            } catch (const std::exception& error) {
                const std::scoped_lock guard(lock);
                if (!firstError) {
                    firstError = std::make_exception_ptr(
                        GenerateError("chunk (" + std::to_string(chunkX) + ", " +
                                      std::to_string(chunkZ) + "): " + error.what()));
                }
                failed.store(true);
            } catch (...) {
                // Not a std::exception, so nothing to add a chunk to: kept
                // whole, because escaping a thread would end the process.
                const std::scoped_lock guard(lock);
                if (!firstError) {
                    firstError = std::current_exception();
                }
                failed.store(true);
            }
        }
        const std::scoped_lock guard(lock);
        add(times, scratch.times);
    };

    const auto helpers =
        static_cast<std::size_t>(std::min<std::size_t>(threads, plan.chunks.size()) - 1U);
    std::vector<std::thread> pool;
    pool.reserve(helpers);
    for (std::size_t i = 0; i < helpers; ++i) {
        pool.emplace_back(work);
    }
    work();
    for (std::thread& thread : pool) {
        thread.join();
    }
    if (firstError) {
        std::rethrow_exception(firstError);
    }
    return payloads;
}

} // namespace

std::span<const DimensionPreset> dimensionPresets() noexcept {
    return kPresets;
}

const DimensionPreset* findDimensionPreset(std::string_view name) noexcept {
    for (const DimensionPreset& preset : kPresets) {
        if (preset.name == name) {
            return &preset;
        }
    }
    return nullptr;
}

std::size_t ChunkRange::count() const noexcept {
    if (maxX < minX || maxZ < minZ) {
        return 0;
    }
    return static_cast<std::size_t>(static_cast<std::int64_t>(maxX) - minX + 1) *
           static_cast<std::size_t>(static_cast<std::int64_t>(maxZ) - minZ + 1);
}

GenerateReport generate(const GenerateRequest& request, const RegionWritten& onRegion) {
    if (request.dimension == nullptr) {
        throw GenerateError("no dimension named");
    }
    const DimensionPreset& preset = *request.dimension;
    const ChunkRange& range = request.chunks;
    if (range.count() == 0) {
        throw GenerateError("the chunk range is empty");
    }
    if (range.minX < kMinChunkCoordinate || range.minZ < kMinChunkCoordinate ||
        range.maxX > kMaxChunkCoordinate || range.maxZ > kMaxChunkCoordinate) {
        throw GenerateError("chunks must lie within the 30 000 000-block world limit, chunk "
                            "coordinates " +
                            std::to_string(kMinChunkCoordinate) + ".." +
                            std::to_string(kMaxChunkCoordinate));
    }
    const unsigned threads = std::max(request.threads, 1U);

    // Refused before anything is generated: a whole region's work lost to a
    // file that was there all along is the cheap failure, not the late one.
    const std::vector<RegionPlan> plans = planRegions(range, request.out);
    if (!request.force) {
        for (const RegionPlan& plan : plans) {
            if (std::filesystem::exists(plan.path)) {
                throw GenerateError(plan.path.string() +
                                    " already exists; --force replaces it, and every chunk it "
                                    "holds outside the requested range is lost with it");
            }
        }
    }

    const std::unique_ptr<world::CompiledDimension> dimension =
        compile(loadPipeline(request.source), preset, request.seed);
    std::filesystem::create_directories(request.out);

    GenerateReport report;
    const Clock::time_point start = Clock::now();
    for (const RegionPlan& plan : plans) {
        std::vector<std::vector<std::byte>> payloads =
            generateRegion(*dimension, preset, plan, threads, report.stages);
        std::map<std::pair<std::int32_t, std::int32_t>, region::ChunkPayload> chunks;
        for (std::size_t i = 0; i < plan.chunks.size(); ++i) {
            // Timestamp zero, not the clock: the same request writes the
            // same bytes, whenever it runs and however many threads it has.
            chunks.emplace(plan.chunks[i],
                           region::ChunkPayload{.timestamp = 0, .nbt = std::move(payloads[i])});
        }
        const Clock::time_point writing = Clock::now();
        region::writeRegion(plan.path, plan.regionX, plan.regionZ, chunks,
                            kLightGrowthPaddingSectors);
        report.stages.write += Clock::now() - writing;
        report.chunks += plan.chunks.size();
        ++report.regions;
        if (onRegion) {
            onRegion(plan.path, plan.chunks.size());
        }
    }
    report.wall = Clock::now() - start;
    return report;
}

} // namespace stratum::cli
