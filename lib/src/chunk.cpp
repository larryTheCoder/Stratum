// Stratum — chunk decoding, from NBT to blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
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
        std::ranges::sort(state.properties);
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

std::vector<std::int64_t> packIndices(const std::vector<std::uint16_t>& indices, int bitsPerEntry) {
    if (bitsPerEntry < 1 || bitsPerEntry > 32) {
        throw FormatError("cannot pack entries " + std::to_string(bitsPerEntry) + " bits wide");
    }
    const auto bits = static_cast<unsigned>(bitsPerEntry);
    const std::size_t perLong = 64U / bits;
    const std::size_t needed = (indices.size() + perLong - 1U) / perLong;
    std::vector<std::uint64_t> words(needed, 0U);
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;

    for (std::size_t i = 0; i < indices.size(); ++i) {
        const std::size_t word = i / perLong;
        const unsigned shift = static_cast<unsigned>(i % perLong) * bits;
        words[word] |= (static_cast<std::uint64_t>(indices[i]) & mask) << shift;
    }

    std::vector<std::int64_t> packed;
    packed.reserve(words.size());
    for (const std::uint64_t word : words) {
        packed.push_back(static_cast<std::int64_t>(word));
    }
    return packed;
}

namespace {

[[nodiscard]] nbt::Tag encodeBlockState(const BlockState& state) {
    nbt::Tag::Compound entry;
    entry.push_back(nbt::NamedTag{.name = "Name", .value = nbt::Tag{state.name}});
    if (!state.properties.empty()) {
        nbt::Tag::Compound properties;
        for (const auto& [key, value] : state.properties) {
            properties.push_back(nbt::NamedTag{.name = key, .value = nbt::Tag{value}});
        }
        entry.push_back(nbt::NamedTag{.name = "Properties", .value = nbt::Tag{properties}});
    }
    return nbt::Tag{entry};
}

/// A paletted container's `palette` list plus, when the palette holds more
/// than one entry, its packed `data` — the single-entry omission is
/// decodePalettedContainer's own rule (chunk.cpp, above), mirrored exactly
/// rather than re-derived.
template<typename PaletteEntry, typename EncodeEntry>
[[nodiscard]] nbt::Tag::Compound encodePalettedContainer(const std::vector<PaletteEntry>& palette,
                                                         const std::vector<std::uint16_t>& indices,
                                                         int floorBits, EncodeEntry&& encodeEntry) {
    nbt::Tag::List paletteList{.elementType = nbt::TagType::End, .elements = {}};
    for (const PaletteEntry& entry : palette) {
        paletteList.elements.push_back(encodeEntry(entry));
    }
    // A list's declared element type has to match what its elements
    // actually are (block palettes are compounds, biome palettes are bare
    // strings) — read from the first one rather than assumed, so this stays
    // correct for whatever EncodeEntry is handed. An empty palette keeps
    // TagType::End, the same declared type nbt::Tag::List{} defaults to.
    if (!paletteList.elements.empty()) {
        paletteList.elementType = paletteList.elements.front().type();
    }

    nbt::Tag::Compound container;
    container.push_back(nbt::NamedTag{.name = "palette", .value = nbt::Tag{paletteList}});
    if (palette.size() > 1) {
        const int bits = bitsPerEntryFor(palette.size(), floorBits);
        container.push_back(
            nbt::NamedTag{.name = "data", .value = nbt::Tag{packIndices(indices, bits)}});
    }
    return container;
}

} // namespace

nbt::Tag encode(const ChunkData& chunk) {
    nbt::Tag::Compound root;
    root.push_back(nbt::NamedTag{.name = "DataVersion", .value = nbt::Tag{chunk.dataVersion}});
    root.push_back(nbt::NamedTag{.name = "xPos", .value = nbt::Tag{chunk.x}});
    root.push_back(nbt::NamedTag{.name = "zPos", .value = nbt::Tag{chunk.z}});
    root.push_back(nbt::NamedTag{.name = "yPos", .value = nbt::Tag{chunk.lowestSection}});
    root.push_back(nbt::NamedTag{.name = "Status", .value = nbt::Tag{chunk.status}});
    root.push_back(nbt::NamedTag{.name = "LastUpdate", .value = nbt::Tag{std::int64_t{0}}});
    root.push_back(nbt::NamedTag{.name = "InhabitedTime", .value = nbt::Tag{std::int64_t{0}}});
    // No light data anywhere (see encode()'s own doc): the server recomputes
    // it, rather than this build guessing at vanilla's sparse per-section
    // storage convention.
    root.push_back(nbt::NamedTag{.name = "isLightOn", .value = nbt::Tag{std::int8_t{0}}});
    root.push_back(nbt::NamedTag{
        .name = "structures",
        .value = nbt::Tag{nbt::Tag::Compound{
            nbt::NamedTag{.name = "References", .value = nbt::Tag{nbt::Tag::Compound{}}},
            nbt::NamedTag{.name = "starts", .value = nbt::Tag{nbt::Tag::Compound{}}}}}});

    if (!chunk.heightmaps.empty()) {
        nbt::Tag::Compound heightmapsCompound;
        const int origin = (chunk.lowestSection * kSectionSize) - 1;
        for (const auto& [kind, heights] : chunk.heightmaps) {
            std::vector<std::uint16_t> raw;
            raw.reserve(heights.size());
            for (const std::optional<int>& height : heights) {
                raw.push_back(
                    static_cast<std::uint16_t>(height.has_value() ? (*height - origin) : 0));
            }
            constexpr int kBitsPerHeight = 9;
            heightmapsCompound.push_back(
                nbt::NamedTag{.name = std::string(heightmapName(kind)),
                              .value = nbt::Tag{packIndices(raw, kBitsPerHeight)}});
        }
        root.push_back(nbt::NamedTag{.name = "Heightmaps", .value = nbt::Tag{heightmapsCompound}});
    }

    nbt::Tag::List sectionsList{.elementType = nbt::TagType::Compound, .elements = {}};
    nbt::Tag::List postProcessing{.elementType = nbt::TagType::List, .elements = {}};
    for (const Section& section : chunk.sections) {
        nbt::Tag::Compound sectionCompound;
        sectionCompound.push_back(
            nbt::NamedTag{.name = "block_states",
                          .value = nbt::Tag{encodePalettedContainer(section.palette, section.blocks,
                                                                    4, encodeBlockState)}});
        if (!section.biomePalette.empty()) {
            sectionCompound.push_back(
                nbt::NamedTag{.name = "biomes",
                              .value = nbt::Tag{encodePalettedContainer(
                                  section.biomePalette, section.biomes, 1,
                                  [](const std::string& name) { return nbt::Tag{name}; })}});
        }
        // Genuinely signed — sections run from -4 upward since 1.18 — the
        // same sign this field carries on the way in (Chunk::decode's own
        // note).
        sectionCompound.push_back(
            nbt::NamedTag{.name = "Y", .value = nbt::Tag{static_cast<std::int8_t>(section.y)}});
        sectionsList.elements.emplace_back(sectionCompound);
        postProcessing.elements.emplace_back(nbt::Tag::List{});
    }
    root.push_back(nbt::NamedTag{.name = "sections", .value = nbt::Tag{sectionsList}});
    root.push_back(nbt::NamedTag{.name = "PostProcessing", .value = nbt::Tag{postProcessing}});
    root.push_back(nbt::NamedTag{.name = "block_entities", .value = nbt::Tag{nbt::Tag::List{}}});
    root.push_back(nbt::NamedTag{.name = "block_ticks", .value = nbt::Tag{nbt::Tag::List{}}});
    root.push_back(nbt::NamedTag{.name = "fluid_ticks", .value = nbt::Tag{nbt::Tag::List{}}});

    return nbt::Tag{root};
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
    if (const nbt::Tag* yPos = root.find("yPos"); yPos != nullptr) {
        chunk.lowestSection_ = yPos->asInt();
    }

    // Kept packed. Decoding all four for every chunk a diff walks would be
    // work almost nobody asked for.
    if (const nbt::Tag* heightmaps = root.find("Heightmaps"); heightmaps != nullptr) {
        for (const Heightmap kind :
             {Heightmap::WorldSurface, Heightmap::OceanFloor, Heightmap::MotionBlocking,
              Heightmap::MotionBlockingNoLeaves}) {
            const nbt::Tag* stored = heightmaps->find(std::string(heightmapName(kind)));
            if (stored != nullptr) {
                chunk.heightmaps_.emplace_back(kind, stored->asLongArray());
            }
        }
    }

    const nbt::Tag* sections = root.find("sections");
    if (sections == nullptr) {
        throw FormatError(describe(chunk.x_, chunk.z_) + " has no sections list");
    }

    for (const nbt::Tag& entry : sections->asList().elements) {
        Section section;
        // Y is genuinely signed — sections run from -4 upward since 1.18 —
        // so the sign extension is the point, not a mistake.
        // NOLINTNEXTLINE(bugprone-signed-char-misuse)
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

    std::ranges::sort(chunk.sections_,
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

    const auto found =
        std::ranges::find_if(sections_, [sectionY](const Section& s) { return s.y == sectionY; });
    if (found == sections_.end()) {
        return nullptr;
    }
    return &found->blockAt(localX, localY, localZ);
}

std::optional<int> Chunk::highestNonAir(int localX, int localZ) const {
    for (const Section& section : std::views::reverse(sections_)) {
        for (int localY = kSectionSize - 1; localY >= 0; --localY) {
            const BlockState& state = section.blockAt(localX, localY, localZ);
            if (state.name != "minecraft:air" && state.name != "minecraft:cave_air" &&
                state.name != "minecraft:void_air") {
                return (section.y * kSectionSize) + localY;
            }
        }
    }
    return std::nullopt;
}

std::string_view heightmapName(Heightmap kind) noexcept {
    switch (kind) {
        case Heightmap::WorldSurface:
            return "WORLD_SURFACE";
        case Heightmap::OceanFloor:
            return "OCEAN_FLOOR";
        case Heightmap::MotionBlocking:
            return "MOTION_BLOCKING";
        case Heightmap::MotionBlockingNoLeaves:
            return "MOTION_BLOCKING_NO_LEAVES";
    }
    return "UNKNOWN";
}

std::optional<std::vector<std::optional<int>>> Chunk::heightmap(Heightmap kind) const {
    const auto found = std::ranges::find_if(
        heightmaps_, [kind](const auto& entry) { return entry.first == kind; });
    if (found == heightmaps_.end()) {
        return std::nullopt;
    }

    // Nine bits holds 0..511, which covers every world height the schema
    // allows plus the "nothing here" zero. The width is a property of the
    // format rather than of the data, so it is not derived from a palette
    // the way a section's is.
    constexpr int kBitsPerHeight = 9;
    constexpr std::size_t kColumns = 256;
    const std::vector<std::uint16_t> raw = unpackIndices(found->second, kBitsPerHeight, kColumns);

    // Stored as `y + 1 - minY`, so that zero is "no qualifying block". See
    // the note on Chunk::heightmap: this offset is measured against the
    // blocks themselves, not assumed.
    const int origin = (lowestSection_ * kSectionSize) - 1;

    std::vector<std::optional<int>> heights;
    heights.reserve(kColumns);
    for (const std::uint16_t value : raw) {
        heights.push_back(value == 0 ? std::optional<int>{}
                                     : std::optional<int>{static_cast<int>(value) + origin});
    }
    return heights;
}

} // namespace stratum::chunk
