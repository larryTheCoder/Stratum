// Stratum — chunk decoding, from NBT to blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stratum::chunk {

namespace {

[[nodiscard]] std::string describe(std::int32_t x, std::int32_t z) {
    return "chunk (" + std::to_string(x) + ", " + std::to_string(z) + ")";
}

[[nodiscard]] BlockState decodeBlockState(const nbt::Tag& entry) {
    BlockState state;
    state.name = entry.at("Name").asString();

    if (const nbt::Tag* properties = entry.find("Properties"); properties != nullptr) {
        for (const nbt::NamedTag& property : properties->asCompound()) {
            state.properties.emplace_back(property.name, property.value.asString());
        }
        // Sorted so that equality is about meaning, not serialisation order.
        std::sort(state.properties.begin(), state.properties.end());
    }
    return state;
}

[[nodiscard]] std::vector<std::uint16_t>
decodePalettedContainer(const nbt::Tag& container, std::size_t entryCount, int floorBits,
                        std::size_t paletteSize, const std::string& what) {
    if (paletteSize == 0) {
        throw FormatError(what + " has an empty palette");
    }

    const nbt::Tag* data = container.find("data");
    if (data == nullptr) {
        // A single-entry palette needs no data: every position is that entry.
        if (paletteSize != 1) {
            throw FormatError(what + " has a palette of " + std::to_string(paletteSize) +
                              " entries but no data array");
        }
        return std::vector<std::uint16_t>(entryCount, 0);
    }

    const int bits = bitsPerEntryFor(paletteSize, floorBits);
    std::vector<std::uint16_t> indices = unpackIndices(data->asLongArray(), bits, entryCount);

    for (const std::uint16_t index : indices) {
        if (static_cast<std::size_t>(index) >= paletteSize) {
            throw FormatError(what + " references palette entry " + std::to_string(index) +
                              " but the palette holds only " + std::to_string(paletteSize));
        }
    }
    return indices;
}

} // namespace

std::string BlockState::toString() const {
    if (properties.empty()) {
        return name;
    }
    std::string text = name;
    text += '[';
    bool first = true;
    for (const auto& [key, value] : properties) {
        if (!first) {
            text += ',';
        }
        first = false;
        text += key;
        text += '=';
        text += value;
    }
    text += ']';
    return text;
}

const BlockState& Section::blockAt(int x, int localY, int z) const {
    const std::size_t index = (static_cast<std::size_t>(localY) * 256U) +
                              (static_cast<std::size_t>(z) * 16U) + static_cast<std::size_t>(x);
    return palette[blocks[index]];
}

int bitsPerEntryFor(std::size_t paletteSize, int floorBits) noexcept {
    int bits = 1;
    while ((std::size_t{1} << static_cast<unsigned>(bits)) < paletteSize) {
        ++bits;
    }
    return std::max(bits, floorBits);
}

std::vector<std::uint16_t> unpackIndices(const std::vector<std::int64_t>& packed, int bitsPerEntry,
                                         std::size_t count) {
    if (bitsPerEntry < 1 || bitsPerEntry > 32) {
        throw FormatError("packed array has an impossible width of " +
                          std::to_string(bitsPerEntry) + " bits per entry");
    }

    const auto bits = static_cast<unsigned>(bitsPerEntry);
    const std::size_t perLong = 64U / bits;
    const std::size_t needed = (count + perLong - 1U) / perLong;
    if (packed.size() != needed) {
        throw FormatError("packed array holds " + std::to_string(packed.size()) + " long(s) but " +
                          std::to_string(count) + " entries of " + std::to_string(bitsPerEntry) +
                          " bits need " + std::to_string(needed));
    }

    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;
    std::vector<std::uint16_t> indices;
    indices.reserve(count);

    for (const std::int64_t word : packed) {
        // Java's longs are signed; the packing is a bit pattern.
        const auto value = static_cast<std::uint64_t>(word);
        for (std::size_t slot = 0; slot < perLong && indices.size() < count; ++slot) {
            // Entries never straddle a long: the top 64 % bits bits of each
            // long are padding. Reading across the boundary instead — the
            // pre-1.16 layout — shifts every block in the chunk.
            const unsigned shift = static_cast<unsigned>(slot) * bits;
            indices.push_back(static_cast<std::uint16_t>((value >> shift) & mask));
        }
    }
    return indices;
}

Chunk Chunk::decode(const nbt::Tag& root) {
    Chunk chunk;

    const nbt::Tag* xPos = root.find("xPos");
    const nbt::Tag* zPos = root.find("zPos");
    if (xPos == nullptr || zPos == nullptr) {
        throw FormatError("chunk NBT has no xPos/zPos; is this a chunk?");
    }
    chunk.x_ = xPos->asInt();
    chunk.z_ = zPos->asInt();

    if (const nbt::Tag* version = root.find("DataVersion"); version != nullptr) {
        chunk.dataVersion_ = version->asInt();
    }
    if (const nbt::Tag* status = root.find("Status"); status != nullptr) {
        chunk.status_ = status->asString();
    }

    const nbt::Tag* sections = root.find("sections");
    if (sections == nullptr) {
        throw FormatError(describe(chunk.x_, chunk.z_) + " has no sections list");
    }

    for (const nbt::Tag& entry : sections->asList().elements) {
        Section section;
        section.y = entry.at("Y").asByte();
        const std::string what =
            describe(chunk.x_, chunk.z_) + " section " + std::to_string(section.y);

        // A section with no block_states is air-only and carries no palette;
        // vanilla writes these for the empty space above terrain.
        const nbt::Tag* states = entry.find("block_states");
        if (states == nullptr) {
            continue;
        }

        for (const nbt::Tag& paletteEntry : states->at("palette").asList().elements) {
            section.palette.push_back(decodeBlockState(paletteEntry));
        }
        section.blocks = decodePalettedContainer(*states, kBlocksPerSection, 4,
                                                 section.palette.size(), what + " block_states");

        if (const nbt::Tag* biomes = entry.find("biomes"); biomes != nullptr) {
            for (const nbt::Tag& biome : biomes->at("palette").asList().elements) {
                section.biomePalette.push_back(biome.asString());
            }
            section.biomes = decodePalettedContainer(*biomes, kBiomesPerSection, 1,
                                                     section.biomePalette.size(), what + " biomes");
        }

        chunk.sections_.push_back(std::move(section));
    }

    std::sort(chunk.sections_.begin(), chunk.sections_.end(),
              [](const Section& lhs, const Section& rhs) { return lhs.y < rhs.y; });
    return chunk;
}

int Chunk::minY() const noexcept {
    return sections_.empty() ? 0 : sections_.front().y * kSectionSize;
}

int Chunk::maxY() const noexcept {
    return sections_.empty() ? -1 : ((sections_.back().y * kSectionSize) + kSectionSize) - 1;
}

const BlockState* Chunk::blockAt(int localX, int y, int localZ) const {
    // floorDiv, not `/`: sections below y=0 are the norm since 1.18, and
    // truncation would fold section -1 onto section 0.
    const int sectionY = javamath::floorDiv(y, kSectionSize);
    const int localY = javamath::floorMod(y, kSectionSize);

    const auto found = std::find_if(sections_.begin(), sections_.end(),
                                    [sectionY](const Section& s) { return s.y == sectionY; });
    if (found == sections_.end()) {
        return nullptr;
    }
    return &found->blockAt(localX, localY, localZ);
}

std::optional<int> Chunk::highestNonAir(int localX, int localZ) const {
    for (auto section = sections_.rbegin(); section != sections_.rend(); ++section) {
        for (int localY = kSectionSize - 1; localY >= 0; --localY) {
            const BlockState& state = section->blockAt(localX, localY, localZ);
            if (state.name != "minecraft:air" && state.name != "minecraft:cave_air" &&
                state.name != "minecraft:void_air") {
                return (section->y * kSectionSize) + localY;
            }
        }
    }
    return std::nullopt;
}

} // namespace stratum::chunk
