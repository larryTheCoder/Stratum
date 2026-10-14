// Stratum — the Nukkit binding's own generation pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The boundary `ext-nukkit/src/jni_bridge.cpp` wraps in JNI, not the JNI
// surface itself: plain C++, built and unit-tested without a JVM. It calls
// into `world::CompiledDimension`, the compile-and-fill core every native
// binding shares, and adds what a Nukkit caller needs that `lib/` does not
// give it on its own:
//
//   * the closed set of Java block states a dimension can place, so the Java
//     side can resolve each one ONCE, at world load, through Nukkit's own
//     `BlockStateMapping` (SPEC §9) and hand the answers back as a
//     `BlockIdTable`;
//   * a fill that only indexes that table, so no block is ever resolved,
//     guessed or substituted while a chunk generates;
//   * Bedrock biome ids per quart (`lib/mapping/`, reused, not re-derived).
//
// SCOPE: noise settings shaped like Nukkit's overworld (y -64..319, which
// is vanilla's overworld, amplified and large_biomes) with a multi-noise
// biome source. Nukkit's other dimensions (the Nether 0..127, the End
// 0..255) are not matched to anything here yet, so settings of any other
// height, and the End's own biome source, are refused by name when a world
// opens (SPEC §8).
#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/world/dimension.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::nukkit {

/// Everything this binding refuses rather than guesses at: a world it cannot
/// open, a block state its table cannot place, an output buffer of the wrong
/// size. Named in the message (SPEC §8). `jni_bridge.cpp` turns it into a
/// Java `StratumGenerationException` rather than letting it cross the JNI
/// boundary.
class NukkitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Nukkit's overworld, `DimensionEnum.OVERWORLD`: y -64..319 inclusive.
inline constexpr std::int32_t kOverworldMinY = -64;
inline constexpr std::int32_t kOverworldHeight = 384;

/// The file a world's frozen pipeline lives in when its generator options
/// name none, beside `level.dat` — the same name `ext/`'s plugin uses.
inline constexpr const char* kBlobFileName = "stratum-pipeline.blob";

/// Freezes the pinned version's datapack rooted at @p versionRoot (its
/// `worldgen/` and `biome_parameters/` as siblings, as `tools/fetch-vanilla`
/// leaves them) into @p blobPath (SPEC §6). Refuses, by path, to replace a
/// file that already exists: a world's pipeline is written once, before the
/// world has a chunk, and never again. Written to a sibling temporary file
/// and renamed into place, so a crash never leaves half a blob behind.
void freezePack(const std::filesystem::path& versionRoot, const std::filesystem::path& blobPath);

/// Java block state id (`mapping::javaBlockStateId`) → Nukkit full block id
/// (`legacyId << 13 | meta`, what `FullChunk.setFullBlockId` takes). Built
/// from what the Java side resolved through Nukkit's own `BlockStateMapping`;
/// this class never resolves anything itself. Immutable once built.
class BlockIdTable {
public:
    /// @p javaStateIds[i] resolved to @p fullIds[i]. Throws `NukkitError` for
    /// spans of different lengths, a Java state id outside the pinned
    /// version's registry, a negative full id, or one state given two
    /// different ids.
    [[nodiscard]] static BlockIdTable fromResolved(std::span<const std::int32_t> javaStateIds,
                                                   std::span<const std::int32_t> fullIds);

    /// The full id of @p javaStateId, or nothing if it was not resolved.
    [[nodiscard]] std::optional<std::int32_t> find(std::uint32_t javaStateId) const noexcept;

    /// How many states resolved.
    [[nodiscard]] std::size_t size() const noexcept { return resolved_; }

private:
    BlockIdTable() = default;

    std::vector<std::int32_t> byJavaState_;
    std::size_t resolved_ = 0;
};

/// Every Java block state a dimension with @p settings can place, deduplicated,
/// in a stable order: air, lava (`level=0`), the default block, the default
/// fluid, the six ore-vein blocks when veins are on, every `block` rule of
/// the surface rules, and the seven terracottas when they use `bandlands`.
///
/// This mirrors what `terrain::ChunkFiller` and `surface::Executor` place —
/// `lib/` does not publish the set itself (a gap, see ext-nukkit/README.md).
/// `Pipeline::fill` refuses, by name, a block outside the table it is given,
/// so a block `lib/` starts placing that this list misses fails loudly rather
/// than silently; the unit tests also fill real chunks and check every
/// palette entry is listed. Throws whatever `surface::RuleGraph::resolve`
/// throws for rules it cannot read.
[[nodiscard]] std::vector<settings::BlockState>
emittableBlockStates(const settings::NoiseSettings& settings);

/// `minecraft:snow_layer[layers=1]`: a Java block state, spelled the way the
/// game's own commands spell one, for messages.
[[nodiscard]] std::string describe(const settings::BlockState& state);

/// One dimension of one world, compiled once and shared by every generation
/// thread Nukkit runs (SPEC §4.1: immutable after compile).
class Pipeline {
public:
    /// One biome the biome source can return, with the Bedrock id
    /// `lib/mapping/` gives it.
    struct Biome {
        data::ResourceLocation java;
        std::int32_t bedrockId = 0;
    };

    /// Compiles noise settings @p settings of @p pipeline with @p biomes for
    /// @p worldSeed. Refuses by name (NukkitError): settings whose height is
    /// not Nukkit's overworld's, the End's biome source, and any biome the
    /// source can return that has no Bedrock id — the nearest-biome fallback
    /// is not a v1 goal, so a biome outside the table is refused when the
    /// world opens, never discovered mid-generation. `lib/`'s own errors
    /// (an unknown settings or parameter list name, an unrunnable rule)
    /// pass through unchanged.
    [[nodiscard]] static std::unique_ptr<Pipeline> compile(freeze::Pipeline pipeline,
                                                           const data::ResourceLocation& settings,
                                                           const world::BiomeSource& biomes,
                                                           std::int64_t worldSeed);

    /// The same from a world's frozen pipeline on disk (`freeze::read`, which
    /// refuses a blob this engine cannot reproduce).
    [[nodiscard]] static std::unique_ptr<Pipeline> open(const std::filesystem::path& blobPath,
                                                        const data::ResourceLocation& settings,
                                                        const world::BiomeSource& biomes,
                                                        std::int64_t worldSeed);

    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    [[nodiscard]] std::int32_t minY() const noexcept;
    [[nodiscard]] std::int32_t height() const noexcept;

    /// `emittableBlockStates` of this dimension's settings, and the Java
    /// state id of each, index for index.
    [[nodiscard]] const std::vector<settings::BlockState>& emittableStates() const noexcept;
    [[nodiscard]] const std::vector<std::uint32_t>& emittableStateIds() const noexcept;

    /// Every biome the biome source can return, sorted by Java id.
    [[nodiscard]] const std::vector<Biome>& emittableBiomes() const noexcept;

    /// Throws NukkitError naming every emittable state @p table does not
    /// resolve. A binding calls this once, when a world opens.
    void requireComplete(const BlockIdTable& table) const;

    /// Fills chunk (@p chunkX, @p chunkZ). Spans are caller-sized:
    /// `16*16*height()` full block ids, index `(ly * 16 + lz) * 16 + lx`
    /// with `ly` counted up from `minY()`; and `16*(height()/4)` Bedrock
    /// biome ids, one per quart, index `(qy * 4 + qz) * 4 + qx`. A quart is
    /// 4x4x4 blocks; Nukkit stores a biome per block, so the caller writes
    /// each quart's id to its 64 blocks.
    ///
    /// When @p fluidUpdates is given, it receives the chunk's fluid updates
    /// (`terrain::ChunkBuffer::fluidUpdates`, in the server's order), each
    /// packed as `lx | lz << 4 | (y - minY()) << 8`.
    ///
    /// Throws NukkitError, naming the block, for a placed state @p table
    /// does not resolve — unreachable after `requireComplete(table)` unless
    /// `emittableBlockStates` has fallen behind `lib/` — and for spans of
    /// the wrong size. A call that throws leaves the spans in an unspecified
    /// state.
    void fill(std::int32_t chunkX, std::int32_t chunkZ, const BlockIdTable& table,
              std::span<std::int32_t> fullBlockIds, std::span<std::int32_t> biomeIds,
              std::vector<std::int32_t>* fluidUpdates = nullptr) const;

private:
    Pipeline();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stratum::nukkit
