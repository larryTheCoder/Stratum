// Stratum — a generated chunk, in the shape PocketMine-MP loads sub-chunks from.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Plain C++ with no PHP in it: the zend shim wraps this, and everything
// about the encoding is testable without a PHP runtime (the same split
// `ext-nukkit/` makes around JNI).
//
// THE TARGET FORMAT is `PalettedBlockArray::fromData(int $bitsPerBlock,
// string $wordArray, array $palette)` from pmmp/ext-chunkutils2 0.3.x — the
// only line PocketMine-MP 5 accepts — read from its source (SPEC §11's
// M5-PMMP entry has the citations):
//
//   * 4096 positions, index `(x << 8) | (z << 4) | y` (XZY);
//   * `bitsPerBlock` one of 0, 1, 2, 3, 4, 5, 6, 8, 16 — seven is refused;
//   * words are `uint32_t`, host-endian, `32 / bitsPerBlock` entries per
//     word packed from the low bit, and the word count is exact per width;
//   * width 0 means one palette entry and no words at all;
//   * the palette holds at most `2^bitsPerBlock` entries.
//
// Emitting the smallest width for a deduplicated palette is not only
// compactness: chunkutils2 marks every `fromData` array for garbage
// collection, and one already at its smallest exact shape is the one it
// does not repack.
//
// Block palettes hold vanilla Java block state ids (`mapping::javaBlockStateId`),
// NOT PocketMine-MP state ids. Those are process-local to PocketMine-MP, so
// the PHP side translates each sub-chunk's few palette entries through
// PocketMine-MP's own deserializer (SPEC §9). Biome palettes hold Bedrock
// biome ids, which PocketMine-MP's `BiomeIds` already are.
#pragma once

#include <stratum/world/dimension.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace stratum::pmmp {

/// A chunk this binding cannot encode: a geometry PocketMine-MP's 24
/// sub-chunks cannot hold, a biome with no Bedrock id, or a block state the
/// mapping table does not know. Named in the message (SPEC §8).
class EncodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t kSubChunkVolume = 4096;

/// PocketMine-MP's chunk: sub-chunk indices -4 to 19, 16 blocks each.
inline constexpr std::int32_t kMinSubChunk = -4;
inline constexpr std::int32_t kMaxSubChunk = 19;

/// One `PalettedBlockArray::fromData` call's arguments.
struct PalettedLayer {
    std::uint8_t bitsPerBlock = 0;
    /// Host-endian: the shim hands these bytes over as they are in memory.
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> palette;

    [[nodiscard]] bool operator==(const PalettedLayer& other) const = default;
};

struct EncodedSubChunk {
    /// PocketMine-MP's sub-chunk index (`y >> 4`).
    std::int32_t index = 0;
    /// Absent for a sub-chunk that is all air — PocketMine-MP's own empty
    /// shape, `new SubChunk(Block::EMPTY_STATE_ID, [], $biomes)`.
    std::optional<PalettedLayer> blocks;
    PalettedLayer biomes;
};

/// One chunk, as PocketMine-MP takes it.
struct EncodedChunk {
    /// All 24 of PocketMine-MP's sub-chunks, -4 to 19, bottom to top — not
    /// only those the dimension's noise height covers. A sub-chunk
    /// `Chunk::__construct` is not given becomes air AND an all-ocean biome
    /// array, so a dimension shorter than PocketMine-MP's column (the End,
    /// y 0..127) would otherwise be ocean above and below its terrain.
    /// Outside the noise height the blocks are air and the biomes are the
    /// dimension's biome source read at each quart's own y
    /// (`CompiledDimension::fillBiomes`' range form).
    std::vector<EncodedSubChunk> subChunks;
    /// The aquifer's fluid updates (spec Q8.1): the positions a Java server
    /// keeps in the chunk's `PostProcessing` lists and ticks once the chunk
    /// loads, which is what makes aquifer water and lava flow. Chunk-local x
    /// and z, world y, in the server's own order
    /// (`terrain::ChunkBuffer::fluidUpdates()`). PocketMine-MP has no such
    /// list, so the plugin schedules a block update at each instead
    /// (fluid_updates.hpp says how they reach it).
    std::vector<terrain::ChunkBuffer::FluidUpdate> fluidUpdates;
};

/// `(x << 8) | (z << 4) | y` within a sub-chunk.
[[nodiscard]] constexpr std::size_t subChunkIndex(int x, int y, int z) noexcept {
    return (static_cast<std::size_t>(x) << 8U) | (static_cast<std::size_t>(z) << 4U) |
           static_cast<std::size_t>(y);
}

/// How many words `fromData` requires at @p bitsPerBlock. Throws EncodeError
/// for a width chunkutils2 does not accept.
[[nodiscard]] std::size_t wordCount(std::uint8_t bitsPerBlock);

/// Packs 4096 values: a palette deduplicated in first-seen order, at the
/// smallest width that holds it.
[[nodiscard]] PalettedLayer encodeLayer(std::span<const std::uint32_t, kSubChunkVolume> values);

/// A sub-chunk's 4 x 4 x 4 quarts of 4 x 4 x 4 positions each.
inline constexpr std::size_t kSubChunkQuarts = 64;

/// `encodeLayer` of a layer that is constant over each quart — a biome
/// layer — from its 64 quart values, quart (qx, qy, qz) at
/// `(qx << 4) | (qz << 2) | qy` (XZY, as subChunkIndex): exactly the layer
/// encodeLayer returns for the 4096 positions each holding its quart's
/// value, palette order included, without materialising them.
[[nodiscard]] PalettedLayer
encodeQuartLayer(std::span<const std::uint32_t, kSubChunkQuarts> quarts);

/// Throws EncodeError, naming the span, unless @p geometry lies within
/// PocketMine-MP's 24 sub-chunks (y -64 to 320) on sub-chunk boundaries.
/// encodeChunk refuses the same; this is the same refusal for a binding to
/// make when it opens a dimension (dimension_registry.hpp), so a dimension
/// that cannot be encoded is refused there — on PocketMine-MP's main thread
/// — rather than at its first chunk, on a worker.
void requireFitsChunk(const settings::NoiseGeometry& geometry);

/// Chunk (@p chunkX, @p chunkZ) of @p dimension, as all 24 PocketMine-MP
/// sub-chunks, bottom to top (EncodedChunk::subChunks says what fills those
/// outside the noise height), plus its fluid updates. Throws EncodeError for
/// a dimension that does not fit PocketMine-MP's chunk (requireFitsChunk),
/// and lets `mapping::BlockMappingError` through, naming the block.
[[nodiscard]] EncodedChunk encodeChunk(const world::CompiledDimension& dimension,
                                       std::int32_t chunkX, std::int32_t chunkZ);

} // namespace stratum::pmmp
