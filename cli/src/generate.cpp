// Stratum — `stratum generate`: region files from the library.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include "generate.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/feature/feature.hpp>
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
#include <span>
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

/// Fills @p blocks and @p biomes with chunk (@p chunkX, @p chunkZ): its
/// terrain, carved where the dimension carves, and its biomes over the
/// stored column.
void fillChunk(const world::CompiledDimension& dimension, const DimensionPreset& column,
               std::int32_t chunkX, std::int32_t chunkZ, terrain::ChunkBuffer& blocks,
               std::span<const data::ResourceLocation*> biomes, StageTimes& times) {
    const Clock::time_point start = Clock::now();
    dimension.fillBlocks(chunkX, chunkZ, blocks);
    const Clock::time_point filled = Clock::now();
    // Handed the blocks just filled: the quarts their surface pass looked
    // up are taken from them, the same biomes (CompiledDimension::fillBiomes).
    dimension.fillBiomes(chunkX, chunkZ, javamath::floorDiv(column.minY, kQuartWidth), biomes,
                         blocks);
    times.fill += filled - start;
    times.biomes += Clock::now() - filled;
}

/// Chunk (@p chunkX, @p chunkZ), holding @p blocks and @p biomes, as the NBT
/// a server stores it as.
[[nodiscard]] std::vector<std::byte>
encodeChunk(const world::CompiledDimension& dimension, const DimensionPreset& column,
            std::int32_t chunkX, std::int32_t chunkZ, const terrain::ChunkBuffer& blocks,
            std::span<const data::ResourceLocation* const> biomes, StageTimes& times) {
    const Clock::time_point start = Clock::now();
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
            const data::ResourceLocation* biome = biomes[firstCell + cell];
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
    times.encode += Clock::now() - start;
    return bytes;
}

/// Chunk (@p chunkX, @p chunkZ) as the NBT a server stores it as.
[[nodiscard]] std::vector<std::byte> generateChunk(const world::CompiledDimension& dimension,
                                                   const DimensionPreset& column,
                                                   std::int32_t chunkX, std::int32_t chunkZ,
                                                   Scratch& scratch) {
    fillChunk(dimension, column, chunkX, chunkZ, scratch.blocks, scratch.biomes, scratch.times);
    return encodeChunk(dimension, column, chunkX, chunkZ, scratch.blocks, scratch.biomes,
                       scratch.times);
}

[[nodiscard]] freeze::Pipeline loadPipeline(const std::filesystem::path& source, const bool carve) {
    if (std::filesystem::is_directory(source)) {
        if (!std::filesystem::is_directory(source / "worldgen")) {
            throw GenerateError(source.string() +
                                " is not a tools/fetch-vanilla version root: it has no "
                                "worldgen/ directory (or pass a frozen pipeline blob)");
        }
        const data::Pack pack = data::Pack::open(source / "worldgen");
        if (!carve) {
            return freeze::resolve(pack, source / "biome_parameters");
        }
        return freeze::resolve(pack, source / "biome_parameters", source / "tags" / "block");
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
    freeze::Pipeline pipeline = freeze::read(blob);
    if (!carve) {
        pipeline.carvers = {};
    }
    return pipeline;
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
    total.decorate += part.decorate;
    total.encode += part.encode;
    total.write += part.write;
}

/// Runs @p body(i, times) for every i below @p count on @p threads threads,
/// each with its own StageTimes, summed into @p times. The first failure
/// stops the rest and is rethrown, named by @p describe(i).
template<typename Body, typename Describe>
void forEachIndex(const std::size_t count, const unsigned threads, StageTimes& times,
                  const Body& body, const Describe& describe) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex lock;
    std::exception_ptr firstError;
    const auto work = [&] {
        StageTimes mine;
        while (!failed.load()) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count) {
                break;
            }
            try {
                body(i, mine);
            } catch (const std::exception& error) {
                const std::scoped_lock guard(lock);
                if (!firstError) {
                    firstError =
                        std::make_exception_ptr(GenerateError(describe(i) + ": " + error.what()));
                }
                failed.store(true);
            } catch (...) {
                const std::scoped_lock guard(lock);
                if (!firstError) {
                    firstError = std::current_exception();
                }
                failed.store(true);
            }
        }
        const std::scoped_lock guard(lock);
        add(times, mine);
    };
    const auto helpers = static_cast<std::size_t>(std::min<std::size_t>(threads, count) - 1U);
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
}

/// A range's chunks decorated: the range and two rings round it filled, the
/// range and one ring decorated (GenerateRequest::features).
class DecoratedRange {
public:
    DecoratedRange(const world::CompiledDimension& dimension, const DimensionPreset& column,
                   const ChunkRange& range, const unsigned threads, StageTimes& times)
        : minX_(range.minX - 2), minZ_(range.minZ - 2),
          width_(static_cast<std::size_t>(std::int64_t{range.maxX} - range.minX + 5)),
          depth_(static_cast<std::size_t>(std::int64_t{range.maxZ} - range.minZ + 5)) {
        const std::size_t count = width_ * depth_;
        const auto biomeCells =
            static_cast<std::size_t>(column.height / kQuartWidth) * kQuartsPerLayer;
        chunks_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            chunks_.push_back(
                Filled{.blocks = terrain::ChunkBuffer(dimension.geometry()),
                       .biomes = std::vector<const data::ResourceLocation*>(biomeCells),
                       .heightmaps = {}});
        }
        forEachIndex(
            count, threads, times,
            [&](const std::size_t i, StageTimes& mine) {
                Filled& chunk = chunks_[i];
                fillChunk(dimension, column, xOf(i), zOf(i), chunk.blocks, chunk.biomes, mine);
                chunk.heightmaps = feature::terrainHeightmaps(chunk.blocks);
            },
            [&](const std::size_t i) {
                return "chunk (" + std::to_string(xOf(i)) + ", " + std::to_string(zOf(i)) + ")";
            });

        // One thread, z-major: what a chunk's features find where a
        // neighbour's reached first is then the same on every run.
        const Clock::time_point start = Clock::now();
        const std::int32_t minQuartY = javamath::floorDiv(column.minY, kQuartWidth);
        for (std::size_t z = 1; z + 1 < depth_; ++z) {
            for (std::size_t x = 1; x + 1 < width_; ++x) {
                std::array<feature::RegionChunk, 9> around{};
                for (std::size_t dz = 0; dz < 3; ++dz) {
                    for (std::size_t dx = 0; dx < 3; ++dx) {
                        Filled& chunk = chunks_[((z + dz - 1) * width_) + x + dx - 1];
                        around.at((dz * 3) + dx) = {.blocks = &chunk.blocks,
                                                    .heightmaps = &chunk.heightmaps,
                                                    .biomes = chunk.biomes,
                                                    .minQuartY = minQuartY};
                    }
                }
                const std::size_t i = (z * width_) + x;
                try {
                    dimension.decorate(xOf(i), zOf(i), around);
                } catch (const std::exception& error) {
                    throw GenerateError("decorating chunk (" + std::to_string(xOf(i)) + ", " +
                                        std::to_string(zOf(i)) + "): " + error.what());
                }
            }
        }
        times.decorate += Clock::now() - start;
    }

    [[nodiscard]] const terrain::ChunkBuffer& blocks(const std::int32_t chunkX,
                                                     const std::int32_t chunkZ) const {
        return chunks_[indexOf(chunkX, chunkZ)].blocks;
    }

    [[nodiscard]] std::span<const data::ResourceLocation* const>
    biomes(const std::int32_t chunkX, const std::int32_t chunkZ) const {
        return chunks_[indexOf(chunkX, chunkZ)].biomes;
    }

private:
    struct Filled {
        terrain::ChunkBuffer blocks;
        std::vector<const data::ResourceLocation*> biomes;
        feature::TerrainHeightmaps heightmaps;
    };

    [[nodiscard]] std::int32_t xOf(const std::size_t index) const noexcept {
        return minX_ + static_cast<std::int32_t>(index % width_);
    }

    [[nodiscard]] std::int32_t zOf(const std::size_t index) const noexcept {
        return minZ_ + static_cast<std::int32_t>(index / width_);
    }

    [[nodiscard]] std::size_t indexOf(const std::int32_t chunkX,
                                      const std::int32_t chunkZ) const noexcept {
        return (static_cast<std::size_t>(chunkZ - minZ_) * width_) +
               static_cast<std::size_t>(chunkX - minX_);
    }

    std::int32_t minX_;
    std::int32_t minZ_;
    std::size_t width_;
    std::size_t depth_;
    std::vector<Filled> chunks_;
};

/// Every chunk of @p plan, generated on @p threads threads; entry i is the
/// NBT of `plan.chunks[i]`, whichever thread made it.
[[nodiscard]] std::vector<std::vector<std::byte>>
generateRegion(const world::CompiledDimension& dimension, const DimensionPreset& column,
               const RegionPlan& plan, unsigned threads, StageTimes& times,
               const DecoratedRange* decorated) {
    std::vector<std::vector<std::byte>> payloads(plan.chunks.size());
    if (decorated != nullptr) {
        // Already filled and decorated: only the encoding is left.
        forEachIndex(
            plan.chunks.size(), threads, times,
            [&](const std::size_t i, StageTimes& mine) {
                const auto [chunkX, chunkZ] = plan.chunks[i];
                payloads[i] = encodeChunk(dimension, column, chunkX, chunkZ,
                                          decorated->blocks(chunkX, chunkZ),
                                          decorated->biomes(chunkX, chunkZ), mine);
            },
            [&](const std::size_t i) {
                return "chunk (" + std::to_string(plan.chunks[i].first) + ", " +
                       std::to_string(plan.chunks[i].second) + ")";
            });
        return payloads;
    }
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

    if (request.features) {
        if (!request.carve) {
            throw GenerateError("features run over carved terrain: --features cannot be "
                                "combined with --no-carvers");
        }
        const std::size_t filled =
            static_cast<std::size_t>(std::int64_t{range.maxX} - range.minX + 5) *
            static_cast<std::size_t>(std::int64_t{range.maxZ} - range.minZ + 5);
        if (filled > kMaxDecoratedChunks) {
            throw GenerateError("--features holds the whole range and two rings of chunks "
                                "round it at once, " +
                                std::to_string(filled) + " chunks here; the most is " +
                                std::to_string(kMaxDecoratedChunks) +
                                ". Generate the range in parts");
        }
    }

    const std::unique_ptr<world::CompiledDimension> dimension =
        compile(loadPipeline(request.source, request.carve), preset, request.seed);
    GenerateReport report;
    if (request.features) {
        const feature::FeatureSet* features = dimension->features();
        if (features == nullptr) {
            throw GenerateError("dimension " + describe(preset) +
                                " has no features to run: " + dimension->featuresBlockedBy());
        }
        report.featuresRun = features->supportedCount();
        for (const feature::FeatureSet::UnsupportedFeature& left : features->unsupported()) {
            report.featuresLeftOut.push_back(left.id.toString() + ": " + left.reason);
        }
    }
    std::filesystem::create_directories(request.out);

    const Clock::time_point start = Clock::now();
    std::optional<DecoratedRange> decorated;
    if (request.features) {
        decorated.emplace(*dimension, preset, range, threads, report.stages);
    }
    for (const RegionPlan& plan : plans) {
        std::vector<std::vector<std::byte>> payloads =
            generateRegion(*dimension, preset, plan, threads, report.stages,
                           decorated.has_value() ? &*decorated : nullptr);
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
