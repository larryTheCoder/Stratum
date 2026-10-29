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
constexpr std::size_t kQuartsPerLayer = 16;

/// PocketMine-MP's column in quart layers: from y -64, 96 of them.
constexpr std::int32_t kMinQuartY = kMinSubChunk * kSubChunkEdge / kQuartWidth;
constexpr auto kQuartLayers =
    static_cast<std::size_t>((kMaxSubChunk + 1 - kMinSubChunk) * kSubChunkEdge / kQuartWidth);
static_assert(kMinQuartY * kQuartWidth == kMinSubChunk * kSubChunkEdge,
              "PocketMine-MP's column starts on a quart boundary");

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

/// Each position's palette offset: at most 4095, the palette holding at
/// most one entry per position.
using Offsets = std::array<std::uint16_t, kSubChunkVolume>;

/// A layer's palette under construction: the values in first-seen order and
/// a map from value to offset — open addressing over a power-of-two table,
/// on the stack while the palette holds up to 128 values and on the heap
/// past that. Any map that answers "which offset has this value" gives the
/// same palette; this one is only cheaper than std::unordered_map.
class PaletteBuilder {
public:
    explicit PaletteBuilder(std::vector<std::uint32_t>& palette) : palette_(palette) {}

    PaletteBuilder(const PaletteBuilder&) = delete;
    PaletteBuilder& operator=(const PaletteBuilder&) = delete;
    PaletteBuilder(PaletteBuilder&&) = delete;
    PaletteBuilder& operator=(PaletteBuilder&&) = delete;
    ~PaletteBuilder() = default;

    /// @p value's offset, appending it to the palette if it is new.
    [[nodiscard]] std::uint16_t offsetOf(const std::uint32_t value) {
        std::size_t slot = home(value);
        while (slots_[slot] != 0) {
            if (values_[slot] == value) {
                return static_cast<std::uint16_t>(slots_[slot] - 1U);
            }
            slot = (slot + 1) & mask_;
        }
        const auto offset = static_cast<std::uint16_t>(palette_.size());
        palette_.push_back(value);
        slots_[slot] = static_cast<std::uint16_t>(offset + 1U);
        values_[slot] = value;
        if (palette_.size() * 2 > mask_ + 1) {
            grow();
        }
        return offset;
    }

private:
    static constexpr std::uint32_t kInlineBits = 8;
    static constexpr std::size_t kInlineSlots = std::size_t{1} << kInlineBits;

    [[nodiscard]] std::size_t home(const std::uint32_t value) const noexcept {
        // Fibonacci hashing: the top bits of a multiply by 2^32 / phi.
        return static_cast<std::size_t>((value * 0x9E37'79B9U) >> (32U - bits_));
    }

    /// Doubles the table and re-enters the palette, offsets unchanged. At
    /// most half full after, so a probe always finds an empty slot.
    void grow() {
        ++bits_;
        mask_ = (std::size_t{1} << bits_) - 1;
        heapSlots_.assign(mask_ + 1, 0);
        heapValues_.assign(mask_ + 1, 0);
        slots_ = heapSlots_.data();
        values_ = heapValues_.data();
        for (std::size_t offset = 0; offset < palette_.size(); ++offset) {
            std::size_t slot = home(palette_[offset]);
            while (slots_[slot] != 0) {
                slot = (slot + 1) & mask_;
            }
            slots_[slot] = static_cast<std::uint16_t>(offset + 1U);
            values_[slot] = palette_[offset];
        }
    }

    std::vector<std::uint32_t>& palette_;
    /// 0 for an empty slot, else the value's offset plus one. A value slot
    /// is read only where its slot is set, so those are left as they are.
    std::array<std::uint16_t, kInlineSlots> inlineSlots_{};
    std::array<std::uint32_t, kInlineSlots> inlineValues_;
    std::vector<std::uint16_t> heapSlots_;
    std::vector<std::uint32_t> heapValues_;
    std::uint16_t* slots_ = inlineSlots_.data();
    std::uint32_t* values_ = inlineValues_.data();
    std::uint32_t bits_ = kInlineBits;
    std::size_t mask_ = kInlineSlots - 1;
};

/// The packed words at width @p Bits: word w holds positions w * perWord up,
/// position i at (i % perWord) * Bits from the low bit — chunkutils2's
/// layout, and what the per-position loop this replaced wrote: the same
/// bits, a word at a time, with no division per position.
template<std::uint32_t Bits>
void packWords(const Offsets& offsets, std::vector<std::uint32_t>& words) {
    constexpr std::size_t kPerWord = kWordBits / Bits;
    constexpr std::size_t kFullWords = kSubChunkVolume / kPerWord;
    constexpr std::size_t kTail = kSubChunkVolume % kPerWord;
    words.resize(kFullWords + (kTail != 0 ? 1U : 0U));
    for (std::size_t w = 0; w < kFullWords; ++w) {
        std::uint32_t word = 0;
        for (std::size_t slot = 0; slot < kPerWord; ++slot) {
            word |= std::uint32_t{offsets[(w * kPerWord) + slot]}
                    << static_cast<std::uint32_t>(slot * Bits);
        }
        words[w] = word;
    }
    if constexpr (kTail != 0) {
        std::uint32_t word = 0;
        for (std::size_t slot = 0; slot < kTail; ++slot) {
            word |= std::uint32_t{offsets[(kFullWords * kPerWord) + slot]}
                    << static_cast<std::uint32_t>(slot * Bits);
        }
        words[kFullWords] = word;
    }
}

/// The layer once its palette and offsets are known: the smallest width
/// that holds the palette, and the words at it.
void finishLayer(PalettedLayer& layer, const Offsets& offsets) {
    layer.bitsPerBlock = widthFor(layer.palette.size());
    switch (layer.bitsPerBlock) {
        case 0:
            layer.words.clear();
            return;
        case 1:
            packWords<1>(offsets, layer.words);
            return;
        case 2:
            packWords<2>(offsets, layer.words);
            return;
        case 3:
            packWords<3>(offsets, layer.words);
            return;
        case 4:
            packWords<4>(offsets, layer.words);
            return;
        case 5:
            packWords<5>(offsets, layer.words);
            return;
        case 6:
            packWords<6>(offsets, layer.words);
            return;
        case 8:
            packWords<8>(offsets, layer.words);
            return;
        case 16:
            packWords<16>(offsets, layer.words);
            return;
        default:
            // widthFor returns only the widths above.
            throw EncodeError("chunkutils2 does not accept " + std::to_string(layer.bitsPerBlock) +
                              " bits per block");
    }
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
    Offsets offsets; // every entry written below
    {
        PaletteBuilder palette(layer.palette);
        // A run of one value — a column of stone, a quart's biome — is
        // looked up once: the offset of the value before is the offset of
        // this one when the two are equal.
        std::uint32_t runValue = values[0];
        std::uint16_t runOffset = palette.offsetOf(runValue);
        for (std::size_t i = 0; i < kSubChunkVolume; ++i) {
            if (values[i] != runValue) {
                runValue = values[i];
                runOffset = palette.offsetOf(runValue);
            }
            offsets[i] = runOffset;
        }
    }
    finishLayer(layer, offsets);
    return layer;
}

PalettedLayer encodeQuartLayer(const std::span<const std::uint32_t, kSubChunkQuarts> quarts) {
    // encodeLayer of the 4096 positions, each holding its quart's value.
    // Position (x, y, z)'s index is x << 8 | z << 4 | y and its quart's is
    // (x / 4) << 4 | (z / 4) << 2 | y / 4, so the first position a quart
    // holds is its corner (4qx, 4qy, 4qz), and the corners' indices rise in
    // the quarts' own index order: a value is first seen, in position
    // order, at the first quart holding it, in quart order. The palette is
    // therefore built over the 64 quarts, and each position takes its
    // quart's offset.
    PalettedLayer layer;
    std::array<std::uint16_t, kSubChunkQuarts> quartOffsets{};
    {
        PaletteBuilder palette(layer.palette);
        for (std::size_t q = 0; q < kSubChunkQuarts; ++q) {
            quartOffsets[q] = palette.offsetOf(quarts[q]);
        }
    }
    if (layer.palette.size() == 1) {
        layer.bitsPerBlock = 0;
        return layer;
    }
    // Position i's quart is ((i >> 10) << 4) | (((i >> 6) & 3) << 2) | ((i >> 2) & 3):
    // every (x, z) column of a quart column holds the same sixteen offsets,
    // so each quart column's is built once and copied to its sixteen.
    Offsets offsets; // every entry written below
    constexpr std::size_t kColumn = 16;
    for (std::size_t qx = 0; qx < 4; ++qx) {
        for (std::size_t qz = 0; qz < 4; ++qz) {
            std::array<std::uint16_t, kColumn> column{};
            for (std::size_t y = 0; y < kColumn; ++y) {
                column[y] = quartOffsets[(qx << 4U) | (qz << 2U) | (y >> 2U)];
            }
            for (std::size_t x = 4 * qx; x < (4 * qx) + 4; ++x) {
                for (std::size_t z = 4 * qz; z < (4 * qz) + 4; ++z) {
                    std::ranges::copy(column, offsets.begin() + static_cast<std::ptrdiff_t>(
                                                                    (x << 8U) | (z << 4U)));
                }
            }
        }
    }
    finishLayer(layer, offsets);
    return layer;
}

void requireFitsChunk(const settings::NoiseGeometry& geometry) {
    if (javamath::floorMod(geometry.minY, kSubChunkEdge) != 0 ||
        javamath::floorMod(geometry.height, kSubChunkEdge) != 0 ||
        geometry.minY < kMinSubChunk * kSubChunkEdge ||
        geometry.maxY() > (kMaxSubChunk + 1) * kSubChunkEdge) {
        throw EncodeError("a dimension spanning y " + std::to_string(geometry.minY) + " to " +
                          std::to_string(geometry.maxY()) +
                          " does not fit PocketMine-MP's 24 sub-chunks (y -64 to 320) on "
                          "sub-chunk boundaries");
    }
}

EncodedChunk encodeChunk(const world::CompiledDimension& dimension, const std::int32_t chunkX,
                         const std::int32_t chunkZ) {
    const settings::NoiseGeometry& geometry = dimension.geometry();
    requireFitsChunk(geometry);

    terrain::ChunkBuffer buffer(geometry);
    dimension.fillBlocks(chunkX, chunkZ, buffer);
    // Translated once per chunk palette entry, not once per block.
    std::vector<std::uint32_t> javaIds;
    javaIds.reserve(buffer.palette().size());
    for (const settings::BlockState& state : buffer.palette()) {
        javaIds.push_back(mapping::javaBlockStateId(state));
    }

    // Biomes over PocketMine-MP's whole column, not only the noise height:
    // a sub-chunk Chunk::__construct is not given becomes ocean, so a
    // shorter dimension's — the End's, y 0..127 — is filled above and below
    // it too. The range form of CompiledDimension::fillBiomes reads the
    // biome source at every y: over the End's y 128..255 that is what the
    // server stores, and below 0 and from 256, where a Java End has no
    // column at all, it is the same rule read on — for vanilla's End, the
    // chunk's one biome, since its erosion reads no y.
    std::vector<const data::ResourceLocation*> javaBiomes(kQuartsPerLayer * kQuartLayers);
    dimension.fillBiomes(chunkX, chunkZ, kMinQuartY, javaBiomes);
    std::vector<std::uint32_t> bedrockBiomes;
    bedrockBiomes.reserve(javaBiomes.size());
    std::unordered_map<const data::ResourceLocation*, std::uint32_t> biomeIdOf;
    // The quart before's biome, already resolved: a run of one biome up a
    // column or along a row is looked up once. Only a biome the map has
    // already resolved is ever reused, so a biome without an id is still
    // refused at its first quart.
    const data::ResourceLocation* lastBiome = nullptr;
    std::uint32_t lastId = 0;
    bool haveLast = false;
    for (const data::ResourceLocation* biome : javaBiomes) {
        if (haveLast && biome == lastBiome) {
            bedrockBiomes.push_back(lastId);
            continue;
        }
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
        lastBiome = biome;
        lastId = found->second;
        haveLast = true;
        bedrockBiomes.push_back(lastId);
    }

    // Every one of PocketMine-MP's sub-chunks. Those outside the noise
    // height are air, as a Java chunk's are there.
    EncodedChunk encoded;
    encoded.subChunks.reserve(static_cast<std::size_t>(kMaxSubChunk + 1 - kMinSubChunk));
    std::array<std::uint32_t, kSubChunkVolume> blocks{};
    std::array<std::uint32_t, kSubChunkQuarts> biomes{};
    for (std::int32_t index = kMinSubChunk; index <= kMaxSubChunk; ++index) {
        const std::int32_t sectionMinY = index * kSubChunkEdge;
        const bool generated =
            sectionMinY >= geometry.minY && sectionMinY + kSubChunkEdge <= geometry.maxY();
        bool allAir = true;
        if (generated) {
            for (int x = 0; x < kSubChunkEdge; ++x) {
                for (int z = 0; z < kSubChunkEdge; ++z) {
                    for (int y = 0; y < kSubChunkEdge; ++y) {
                        // Palette index 0 is always air: ChunkBuffer starts
                        // there.
                        const std::uint16_t paletteIndex =
                            buffer.paletteIndexAt(x, sectionMinY + y, z);
                        allAir = allAir && paletteIndex == 0;
                        blocks[subChunkIndex(x, y, z)] = javaIds[paletteIndex];
                    }
                }
            }
        }

        // The sub-chunk's 64 quarts, in encodeQuartLayer's order: its four
        // quart layers are PocketMine-MP's column layers from
        // floorDiv(sectionMinY, 4) - kMinQuartY, which is 4 * (index -
        // kMinSubChunk) since sectionMinY is a multiple of 16.
        const auto firstLayer =
            static_cast<std::size_t>(javamath::floorDiv(sectionMinY, kQuartWidth) - kMinQuartY);
        for (std::size_t qx = 0; qx < 4; ++qx) {
            for (std::size_t qz = 0; qz < 4; ++qz) {
                for (std::size_t qy = 0; qy < 4; ++qy) {
                    biomes[(qx << 4U) | (qz << 2U) | qy] =
                        bedrockBiomes[((((firstLayer + qy) * 4U) + qz) * 4U) + qx];
                }
            }
        }
        EncodedSubChunk sub;
        sub.index = index;
        if (!allAir) {
            sub.blocks = encodeLayer(blocks);
        }
        sub.biomes = encodeQuartLayer(biomes);
        encoded.subChunks.push_back(std::move(sub));
    }
    encoded.fluidUpdates = buffer.fluidUpdates();
    return encoded;
}

} // namespace stratum::pmmp
