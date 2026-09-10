// Stratum — chunk encoding tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// encode()'s own claim is round-trip fidelity against decode(), which is
// independently tested (chunk_test.cpp, against bytes built by hand) — so a
// round trip through encode() here is a genuine check of the writer, not a
// tautology against itself.

#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/nbt/writer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::chunk::BlockState;
using stratum::chunk::Chunk;
using stratum::chunk::ChunkData;
using stratum::chunk::encode;
using stratum::chunk::FormatError;
using stratum::chunk::Heightmap;
using stratum::chunk::packIndices;
using stratum::chunk::Section;
using stratum::chunk::unpackIndices;

namespace {

[[nodiscard]] BlockState block(const std::string& name) {
    return BlockState{.name = name, .properties = {}};
}

} // namespace

TEST_CASE("packIndices is unpackIndices' exact inverse", "[chunk][writer]") {
    // Sizes either side of a long's boundary at each width, so the "entries
    // never straddle a long" padding rule is exercised both where it does
    // nothing (an exact multiple) and where it leaves a remainder.
    for (const int bits : {1, 2, 4, 5, 8, 9, 15}) {
        const std::size_t perLong = 64U / static_cast<unsigned>(bits);
        for (const std::size_t count :
             {perLong - (perLong > 1 ? 1 : 0), perLong, perLong + 1, (perLong * 3) + 1}) {
            if (count == 0) {
                continue;
            }
            std::vector<std::uint16_t> indices;
            const std::uint16_t maxValue = static_cast<std::uint16_t>((1U << bits) - 1U);
            for (std::size_t i = 0; i < count; ++i) {
                indices.push_back(static_cast<std::uint16_t>(i % (maxValue + 1U)));
            }
            const auto packed = packIndices(indices, bits);
            const auto roundTripped = unpackIndices(packed, bits, count);
            CHECK(roundTripped == indices);
        }
    }
}

TEST_CASE("packIndices refuses an impossible width", "[chunk][writer]") {
    CHECK_THROWS_AS(packIndices({0, 1}, 0), FormatError);
    CHECK_THROWS_AS(packIndices({0, 1}, 33), FormatError);
}

TEST_CASE("a chunk round-trips through encode and decode", "[chunk][writer]") {
    Section bottom;
    bottom.y = -4;
    bottom.palette = {block("minecraft:bedrock"), block("minecraft:stone")};
    bottom.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    bottom.blocks[1] = 1; // one stone block, so the palette is genuinely used
    bottom.biomePalette = {"minecraft:plains"};
    bottom.biomes.assign(stratum::chunk::kBiomesPerSection, 0);

    Section top;
    top.y = -3;
    top.palette = {block("minecraft:air")}; // single-entry: no "data" to write
    top.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    top.biomePalette = {"minecraft:plains"};
    top.biomes.assign(stratum::chunk::kBiomesPerSection, 0);

    ChunkData data;
    data.x = 3;
    data.z = -5;
    data.dataVersion = 4671;
    data.lowestSection = -4;
    data.sections = {bottom, top};

    std::vector<std::optional<int>> worldSurface(256, std::optional<int>{-64});
    worldSurface[0] = std::nullopt;
    data.heightmaps.emplace_back(Heightmap::WorldSurface, worldSurface);

    const Chunk decoded = Chunk::decode(encode(data));

    CHECK(decoded.x() == 3);
    CHECK(decoded.z() == -5);
    CHECK(decoded.dataVersion() == 4671);
    CHECK(decoded.lowestSection() == -4);
    CHECK(decoded.status() == "minecraft:full");
    REQUIRE(decoded.sections().size() == 2U);

    // The one stone block landed at local (1, 0, 0) of the bottom section —
    // blocks are laid out YZX (Section::blockAt's own doc).
    CHECK(decoded.blockAt(1, -64, 0)->name == "minecraft:stone");
    CHECK(decoded.blockAt(0, -64, 0)->name == "minecraft:bedrock");
    // y = -48 is the bottom of the next section up (y = -3), the "top"
    // section above, which is uniformly air.
    CHECK(decoded.blockAt(0, -48, 0)->name == "minecraft:air");

    const auto heightmap = decoded.heightmap(Heightmap::WorldSurface);
    REQUIRE(heightmap.has_value());
    CHECK((*heightmap)[0] == std::nullopt);
    CHECK((*heightmap)[1] == std::optional<int>{-64});
}

TEST_CASE("a chunk round-trips through actual NBT bytes, not just in-memory Tags",
          "[chunk][writer]") {
    // Chunk::decode() reads a List's ELEMENTS directly and never looks at
    // its declared elementType, so a round trip through decode() alone
    // cannot catch a paletted container whose declared type disagrees with
    // what its elements actually are — exactly what happened here: biome
    // palettes are bare strings, encodePalettedContainer() hard-coded
    // Compound for every paletted container regardless, and a real Java
    // server's own NBT reader (which DOES care) failed to load the result
    // with "EOFException" / "unknown tag type 20", corrupted from that byte
    // on. This goes through nbt::write() and nbt::read() for real, so it
    // would have caught that the moment it was written this way.
    Section section;
    section.y = 0;
    section.palette = {block("minecraft:stone")};
    section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    // Two entries: unlike a single-entry palette (no "data" to get wrong),
    // this also exercises the packed "data" LongArray alongside the list.
    section.biomePalette = {"minecraft:plains", "minecraft:desert"};
    section.biomes.assign(stratum::chunk::kBiomesPerSection, 0);
    section.biomes[1] = 1;

    ChunkData data;
    data.sections = {section};

    const auto bytes = stratum::nbt::write("", encode(data));
    const auto document = stratum::nbt::read(bytes);
    const Chunk decoded = Chunk::decode(document.root);

    REQUIRE(decoded.sections().size() == 1U);
    CHECK(decoded.sections().front().biomePalette ==
          std::vector<std::string>{"minecraft:plains", "minecraft:desert"});
}

TEST_CASE("a status other than the default is written verbatim", "[chunk][writer]") {
    ChunkData data;
    data.status = "minecraft:noise";
    const Chunk decoded = Chunk::decode(encode(data));
    CHECK(decoded.status() == "minecraft:noise");
}
