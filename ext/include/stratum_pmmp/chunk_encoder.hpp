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

/// Chunk (@p chunkX, @p chunkZ) of @p dimension, as every PocketMine-MP
/// sub-chunk the dimension's height covers, bottom to top. Throws
/// EncodeError for a dimension that does not fit PocketMine-MP's chunk, and
/// lets `mapping::BlockMappingError` through, naming the block.
[[nodiscard]] std::vector<EncodedSubChunk> encodeChunk(const world::CompiledDimension& dimension,
                                                       std::int32_t chunkX, std::int32_t chunkZ);

} // namespace stratum::pmmp
