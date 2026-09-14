// Stratum — a generated chunk, in the shape PocketMine-MP loads sub-chunks from.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/javamath.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/terrain/filler.hpp>

#include <stratum_pmmp/chunk_encoder.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>

namespace stratum::pmmp {

namespace {

constexpr std::int32_t kSubChunkEdge = 16;
constexpr std::int32_t kQuartWidth = 4;
constexpr std::uint32_t kWordBits = 32;

/// The widths chunkutils2 0.3.x accepts above zero, smallest first.
constexpr std::array<std::uint8_t, 8> kWidths = {1, 2, 3, 4, 5, 6, 8, 16};

[[nodiscard]] std::uint8_t widthFor(std::size_t paletteSize) {
    if (paletteSize == 1) {
        return 0;
    }
    for (const std::uint8_t width : kWidths) {
        if ((std::size_t{1} << width) >= paletteSize) {
            return width;
        }
    }
    throw EncodeError("a sub-chunk palette of " + std::to_string(paletteSize) +
                      " entries does not fit any width chunkutils2 accepts");
}

} // namespace

std::size_t wordCount(const std::uint8_t bitsPerBlock) {
    if (bitsPerBlock == 0) {
        return 0;
    }
    if (std::ranges::find(kWidths, bitsPerBlock) == kWidths.end()) {
        throw EncodeError("chunkutils2 does not accept " + std::to_string(bitsPerBlock) +
                          " bits per block");
    }
    const std::size_t perWord = kWordBits / bitsPerBlock;
    return (kSubChunkVolume + perWord - 1) / perWord;
}

PalettedLayer encodeLayer(const std::span<const std::uint32_t, kSubChunkVolume> values) {
    PalettedLayer layer;
    std::unordered_map<std::uint32_t, std::uint32_t> offsetOf;
    std::array<std::uint32_t, kSubChunkVolume> offsets{};
    for (std::size_t i = 0; i < kSubChunkVolume; ++i) {
        const auto [entry, inserted] =
            offsetOf.try_emplace(values[i], static_cast<std::uint32_t>(layer.palette.size()));
        if (inserted) {
            layer.palette.push_back(values[i]);
        }
        offsets[i] = entry->second;
    }

    layer.bitsPerBlock = widthFor(layer.palette.size());
    layer.words.assign(wordCount(layer.bitsPerBlock), 0);
    if (layer.bitsPerBlock == 0) {
        return layer;
    }
    const std::size_t perWord = kWordBits / layer.bitsPerBlock;
    for (std::size_t i = 0; i < kSubChunkVolume; ++i) {
        const auto shift = static_cast<std::uint32_t>((i % perWord) * layer.bitsPerBlock);
        layer.words[i / perWord] |= offsets[i] << shift;
    }
    return layer;
}

std::vector<EncodedSubChunk> encodeChunk(const world::CompiledDimension& dimension,
                                         const std::int32_t chunkX, const std::int32_t chunkZ) {
    const settings::NoiseGeometry& geometry = dimension.geometry();
    if (javamath::floorMod(geometry.minY, kSubChunkEdge) != 0 ||
        javamath::floorMod(geometry.height, kSubChunkEdge) != 0 ||
        geometry.minY < kMinSubChunk * kSubChunkEdge ||
        geometry.maxY() > (kMaxSubChunk + 1) * kSubChunkEdge) {
        throw EncodeError("a dimension spanning y " + std::to_string(geometry.minY) + " to " +
                          std::to_string(geometry.maxY()) +
                          " does not fit PocketMine-MP's 24 sub-chunks (y -64 to 320) on "
                          "sub-chunk boundaries");
    }

    terrain::ChunkBuffer buffer(geometry);
    dimension.fillBlocks(chunkX, chunkZ, buffer);
    // Translated once per chunk palette entry, not once per block.
    std::vector<std::uint32_t> javaIds;
    javaIds.reserve(buffer.palette().size());
    for (const settings::BlockState& state : buffer.palette()) {
        javaIds.push_back(mapping::javaBlockStateId(state));
    }

    const std::int32_t quartsHigh = javamath::floorDiv(geometry.height, kQuartWidth);
    std::vector<const data::ResourceLocation*> javaBiomes(16U *
                                                          static_cast<std::size_t>(quartsHigh));
    dimension.fillBiomes(chunkX, chunkZ, javaBiomes);
    std::vector<std::uint32_t> bedrockBiomes;
    bedrockBiomes.reserve(javaBiomes.size());
    std::unordered_map<const data::ResourceLocation*, std::uint32_t> biomeIdOf;
    for (const data::ResourceLocation* biome : javaBiomes) {
        auto found = biomeIdOf.find(biome);
        if (found == biomeIdOf.end()) {
            const std::optional<std::int32_t> id = mapping::bedrockBiomeId(*biome);
            if (!id.has_value()) {
                throw EncodeError("biome '" + biome->toString() +
                                  "' has no Bedrock id in this build's table (a custom or "
                                  "datapack biome; the nearest-biome fallback is not built yet)");
            }
            found = biomeIdOf.emplace(biome, static_cast<std::uint32_t>(*id)).first;
        }
        bedrockBiomes.push_back(found->second);
    }

    const std::int32_t firstIndex = javamath::floorDiv(geometry.minY, kSubChunkEdge);
    const std::int32_t sections = geometry.height / kSubChunkEdge;
    std::vector<EncodedSubChunk> encoded;
    encoded.reserve(static_cast<std::size_t>(sections));
    std::array<std::uint32_t, kSubChunkVolume> blocks{};
    std::array<std::uint32_t, kSubChunkVolume> biomes{};
    for (std::int32_t section = 0; section < sections; ++section) {
        const std::int32_t sectionMinY = (firstIndex + section) * kSubChunkEdge;
        bool allAir = true;
        for (int x = 0; x < kSubChunkEdge; ++x) {
            for (int z = 0; z < kSubChunkEdge; ++z) {
                for (int y = 0; y < kSubChunkEdge; ++y) {
                    const std::size_t at = subChunkIndex(x, y, z);
                    // Palette index 0 is always air: ChunkBuffer starts there.
                    const std::uint16_t paletteIndex = buffer.paletteIndexAt(x, sectionMinY + y, z);
                    allAir = allAir && paletteIndex == 0;
                    blocks[at] = javaIds[paletteIndex];

                    const auto quartY =
                        static_cast<std::size_t>((sectionMinY + y - geometry.minY) / kQuartWidth);
                    const std::size_t quart =
                        ((quartY * 4U) + static_cast<std::size_t>(z / kQuartWidth)) * 4U +
                        static_cast<std::size_t>(x / kQuartWidth);
                    biomes[at] = bedrockBiomes[quart];
                }
            }
        }
        EncodedSubChunk sub;
        sub.index = firstIndex + section;
        if (!allAir) {
            sub.blocks = encodeLayer(blocks);
        }
        sub.biomes = encodeLayer(biomes);
        encoded.push_back(std::move(sub));
    }
    return encoded;
}

} // namespace stratum::pmmp
