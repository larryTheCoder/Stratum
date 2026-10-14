// Stratum — `stratum generate`: region files from the library.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Generates a chunk range of one dimension through `world::CompiledDimension`
// — the path both native bindings generate through — and writes it as Anvil
// region files (`r.<x>.<z>.mca`) that `stratum diff`, this repository's
// region reader and the vanilla server all read. The output is Java block
// space only: block states and biome identifiers, before any Bedrock mapping
// (SPEC §7, §9).
//
// Everything this file does is assembly: the terrain, the biomes and the
// fluid updates come from `CompiledDimension`, the chunk NBT from
// `chunk::encode` and the container from `region::writeRegion`. What it adds
// is the shape a server stores a chunk in — the dimension's whole stored
// column rather than its noise height, and the four heightmaps — which
// nothing in lib/ assembles yet (`tools/analysis/generate-world.cpp` carries
// an older copy for the overworld alone).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace stratum::cli {

/// Raised for a request that is well formed and cannot be met: a dimension
/// the pipeline cannot compile, a column it does not fit, an output that
/// already exists. The message names what and why.
class GenerateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One dimension `stratum generate` can be asked for, by name: the noise
/// settings and biome source vanilla's world presets pair, and the column
/// its dimension type stores.
///
/// The column is the caller's to say because nothing the library holds
/// says it: a frozen pipeline carries noise settings, not dimension types
/// (SPEC §6), and the two differ — the End's noise settings generate y
/// 0..127 while its dimension type stores sections, and biomes, for y
/// 0..255. A server writes every section of the stored column; a chunk
/// written over the noise height alone would lack the End's upper biomes.
struct DimensionPreset {
    /// What `--dimension` takes.
    std::string_view name;
    /// The noise settings, by id.
    std::string_view noiseSettings;
    /// The biome source, in `world::BiomeSource::parse`'s spelling.
    std::string_view biomeSource;
    /// The stored column: lowest block y and height, both whole sections.
    std::int32_t minY = 0;
    std::int32_t height = 0;
};

/// Every name `--dimension` accepts, in the order usage lists them. Whether
/// the library can compile one is the library's to say, by name, when it is
/// asked to: the table does not second-guess it.
[[nodiscard]] std::span<const DimensionPreset> dimensionPresets() noexcept;

/// The preset called @p name, or nullptr.
[[nodiscard]] const DimensionPreset* findDimensionPreset(std::string_view name) noexcept;

/// The world limit, in chunks: vanilla's world border stops 30 000 000
/// blocks from the origin. Coordinates past it are refused rather than
/// generated, which also keeps every block coordinate inside an int32.
inline constexpr std::int32_t kMinChunkCoordinate = -1'875'000;
inline constexpr std::int32_t kMaxChunkCoordinate = 1'874'999;

/// A rectangle of chunks, both corners included.
struct ChunkRange {
    std::int32_t minX = 0;
    std::int32_t minZ = 0;
    std::int32_t maxX = 0;
    std::int32_t maxZ = 0;

    [[nodiscard]] std::size_t count() const noexcept;
};

struct GenerateRequest {
    /// A `tools/fetch-vanilla` version root (holding `worldgen/` and
    /// `biome_parameters/`), or a frozen pipeline blob (SPEC §6).
    std::filesystem::path source;
    const DimensionPreset* dimension = nullptr;
    std::int64_t seed = 0;
    ChunkRange chunks;
    /// Where `r.<x>.<z>.mca` go; created if missing.
    std::filesystem::path out;
    /// Generation threads. The bytes written do not depend on it.
    unsigned threads = 1;
    /// Replace region files that already exist, rather than refusing.
    bool force = false;
};

/// Time spent in each stage, summed over every thread — so per chunk it is
/// what one generation thread spends on a chunk, whatever --threads said.
struct StageTimes {
    /// `CompiledDimension::fillBlocks`.
    std::chrono::nanoseconds fill{};
    /// `CompiledDimension::fillBiomes` over the stored column.
    std::chrono::nanoseconds biomes{};
    /// Sections, heightmaps and NBT: `chunk::encode` and `nbt::write`.
    std::chrono::nanoseconds encode{};
    /// `region::writeRegion`: compression and the file, on one thread.
    std::chrono::nanoseconds write{};
};

struct GenerateReport {
    std::size_t chunks = 0;
    std::size_t regions = 0;
    StageTimes stages;
    /// From the first chunk to the last region on disk, pipeline loading and
    /// compiling excluded.
    std::chrono::nanoseconds wall{};
};

/// Called once a region file is on disk, with its path and chunk count.
using RegionWritten = std::function<void(const std::filesystem::path&, std::size_t)>;

/// Generates @p request's chunks and writes the region files that hold them,
/// a region at a time; a region the range covers in part holds only the
/// chunks asked for. Every region file the request would write is checked
/// for before anything is generated: one that exists is refused unless
/// `force` is set. Throws GenerateError naming what is wrong, and lets the
/// library's own errors through with the dimension named; regions written
/// before a failure stay on disk.
[[nodiscard]] GenerateReport generate(const GenerateRequest& request,
                                      const RegionWritten& onRegion);

} // namespace stratum::cli
