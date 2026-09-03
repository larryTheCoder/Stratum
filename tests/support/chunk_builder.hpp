// Stratum — synthetic chunk NBT for tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Real region files are Mojang-derived and never committed (SPEC §12), so
// chunk-level tests build their own. The packing here mirrors the format's
// rules deliberately, including that entries never straddle a long.

#pragma once

#include "nbt_writer.hpp"

#include <stratum/chunk/chunk.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace stratum::test {

/// Packs palette indices the way vanilla does: @p bitsPerEntry each, no
/// entry straddling a long, unused high bits left as padding.
[[nodiscard]] inline std::vector<std::uint64_t>
packIndices(const std::vector<std::uint16_t>& values, int bitsPerEntry) {
    const auto bits = static_cast<unsigned>(bitsPerEntry);
    const std::size_t perLong = 64U / bits;
    const std::size_t longs = (values.size() + perLong - 1U) / perLong;

    std::vector<std::uint64_t> packed(longs, 0U);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t word = i / perLong;
        const unsigned shift = static_cast<unsigned>(i % perLong) * bits;
        packed[word] |= static_cast<std::uint64_t>(values[i]) << shift;
    }
    return packed;
}

struct SectionSpec {
    int y = 0;
    std::vector<std::string> palette{"minecraft:air"};
    /// One palette index per block, in YZX order. Empty means "single-entry
    /// palette, no data array", which is what vanilla writes for a uniform
    /// section.
    std::vector<std::uint16_t> blocks;
    std::vector<std::string> biomePalette;
    std::vector<std::uint16_t> biomes;
};

/// One stored heightmap: 256 columns in ZX order, as vanilla writes them —
/// `y + 1 - minY`, so that zero means the column holds nothing.
struct HeightmapSpec {
    std::string name;
    std::vector<std::uint16_t> stored;
};

/// Writes chunk NBT of the shape Chunk::decode expects.
///
/// @p yPos is written whenever it is given. It is deliberately separate from
/// the sections: vanilla's lowest section and its yPos agree, but the
/// heightmap origin is defined by yPos alone, and a test that could not tell
/// them apart could not tell a decoder that used the wrong one.
[[nodiscard]] inline NbtWriter buildChunkNbt(std::int32_t chunkX, std::int32_t chunkZ,
                                             const std::vector<SectionSpec>& sections,
                                             std::int32_t dataVersion = 4440,
                                             std::optional<std::int32_t> yPos = std::nullopt,
                                             const std::vector<HeightmapSpec>& heightmaps = {}) {
    NbtWriter writer;
    writer.named(nbt::TagType::Compound, "");
    writer.named(nbt::TagType::Int, "DataVersion").u32(static_cast<std::uint32_t>(dataVersion));
    writer.named(nbt::TagType::Int, "xPos").u32(static_cast<std::uint32_t>(chunkX));
    writer.named(nbt::TagType::Int, "zPos").u32(static_cast<std::uint32_t>(chunkZ));
    if (yPos.has_value()) {
        writer.named(nbt::TagType::Int, "yPos").u32(static_cast<std::uint32_t>(*yPos));
    }
    writer.named(nbt::TagType::String, "Status").str("minecraft:full");

    if (!heightmaps.empty()) {
        writer.named(nbt::TagType::Compound, "Heightmaps");
        for (const HeightmapSpec& map : heightmaps) {
            const std::vector<std::uint64_t> packed = packIndices(map.stored, 9);
            writer.named(nbt::TagType::LongArray, map.name)
                .u32(static_cast<std::uint32_t>(packed.size()));
            for (const std::uint64_t word : packed) {
                writer.u64(word);
            }
        }
        writer.end(); // Heightmaps
    }
    writer.named(nbt::TagType::List, "sections")
        .type(nbt::TagType::Compound)
        .u32(static_cast<std::uint32_t>(sections.size()));

    for (const SectionSpec& section : sections) {
        writer.named(nbt::TagType::Byte, "Y").u8(static_cast<std::uint8_t>(section.y));

        writer.named(nbt::TagType::Compound, "block_states");
        writer.named(nbt::TagType::List, "palette")
            .type(nbt::TagType::Compound)
            .u32(static_cast<std::uint32_t>(section.palette.size()));
        for (const std::string& name : section.palette) {
            writer.named(nbt::TagType::String, "Name").str(name);
            writer.end();
        }
        if (!section.blocks.empty()) {
            const int bits = chunk::bitsPerEntryFor(section.palette.size(), 4);
            const std::vector<std::uint64_t> packed = packIndices(section.blocks, bits);
            writer.named(nbt::TagType::LongArray, "data")
                .u32(static_cast<std::uint32_t>(packed.size()));
            for (const std::uint64_t word : packed) {
                writer.u64(word);
            }
        }
        writer.end(); // block_states

        if (!section.biomePalette.empty()) {
            writer.named(nbt::TagType::Compound, "biomes");
            writer.named(nbt::TagType::List, "palette")
                .type(nbt::TagType::String)
                .u32(static_cast<std::uint32_t>(section.biomePalette.size()));
            for (const std::string& name : section.biomePalette) {
                writer.str(name);
            }
            if (!section.biomes.empty()) {
                const int bits = chunk::bitsPerEntryFor(section.biomePalette.size(), 1);
                const std::vector<std::uint64_t> packed = packIndices(section.biomes, bits);
                writer.named(nbt::TagType::LongArray, "data")
                    .u32(static_cast<std::uint32_t>(packed.size()));
                for (const std::uint64_t word : packed) {
                    writer.u64(word);
                }
            }
            writer.end(); // biomes
        }

        writer.end(); // section
    }

    writer.end(); // root
    return writer;
}

/// A section filled entirely with one block.
[[nodiscard]] inline SectionSpec uniformSection(int y, const std::string& block) {
    return SectionSpec{y, {block}, {}, {}, {}};
}

} // namespace stratum::test
