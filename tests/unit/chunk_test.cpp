// Stratum — chunk decoding tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The packing rules here are the ones that shift every block in a chunk when
// they are got wrong, so they are tested directly rather than only through
// the decoder.

#include "support/chunk_builder.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using stratum::chunk::bitsPerEntryFor;
using stratum::chunk::Chunk;
using stratum::chunk::FormatError;
using stratum::chunk::unpackIndices;
using stratum::test::buildChunkNbt;
using stratum::test::packIndices;
using stratum::test::SectionSpec;
using stratum::test::uniformSection;

[[nodiscard]] Chunk decode(const stratum::test::NbtWriter& writer) {
    return Chunk::decode(stratum::nbt::read(writer.span()).root);
}

[[nodiscard]] std::vector<std::int64_t> toSigned(const std::vector<std::uint64_t>& packed) {
    std::vector<std::int64_t> signedWords;
    signedWords.reserve(packed.size());
    for (const std::uint64_t word : packed) {
        signedWords.push_back(static_cast<std::int64_t>(word));
    }
    return signedWords;
}

} // namespace

TEST_CASE("palette width follows the size and the floor", "[chunk]") {
    // Blocks never go below 4 bits; biomes never below 1.
    CHECK(bitsPerEntryFor(1, 4) == 4);
    CHECK(bitsPerEntryFor(16, 4) == 4);
    CHECK(bitsPerEntryFor(17, 4) == 5);
    CHECK(bitsPerEntryFor(32, 4) == 5);
    CHECK(bitsPerEntryFor(33, 4) == 6);
    CHECK(bitsPerEntryFor(256, 4) == 8);
    CHECK(bitsPerEntryFor(257, 4) == 9);

    CHECK(bitsPerEntryFor(1, 1) == 1);
    CHECK(bitsPerEntryFor(2, 1) == 1);
    CHECK(bitsPerEntryFor(3, 1) == 2);
    CHECK(bitsPerEntryFor(5, 1) == 3);
}

TEST_CASE("packed entries never straddle a long", "[chunk][landmine]") {
    // The 1.16 change: at 5 bits, 12 entries fit in a long and the top 4 bits
    // are padding. Reading across the boundary — the older layout — shifts
    // every block after the first long.
    std::vector<std::uint16_t> values;
    for (std::uint16_t i = 0; i < 24; ++i) {
        values.push_back(static_cast<std::uint16_t>(i % 20));
    }

    const std::vector<std::uint64_t> packed = packIndices(values, 5);
    REQUIRE(packed.size() == 2U); // 12 entries per long, so 24 needs exactly 2

    const std::vector<std::uint16_t> unpacked = unpackIndices(toSigned(packed), 5, values.size());
    CHECK(unpacked == values);

    // The padding bits really are unused: setting them changes nothing.
    std::vector<std::uint64_t> padded = packed;
    padded[0] |= 0xF000000000000000ULL;
    padded[1] |= 0xF000000000000000ULL;
    CHECK(unpackIndices(toSigned(padded), 5, values.size()) == unpacked);
}

TEST_CASE("packed entries round-trip at every width a chunk uses", "[chunk]") {
    for (int bits = 4; bits <= 12; ++bits) {
        CAPTURE(bits);
        const auto limit = static_cast<std::uint16_t>((1U << static_cast<unsigned>(bits)) - 1U);
        std::vector<std::uint16_t> values(stratum::chunk::kBlocksPerSection);
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<std::uint16_t>(i % (limit + 1U));
        }
        const std::vector<std::uint16_t> unpacked =
            unpackIndices(toSigned(packIndices(values, bits)), bits, values.size());
        CHECK(unpacked == values);
    }
}

TEST_CASE("a long array of the wrong size is refused", "[chunk][malformed]") {
    const std::vector<std::int64_t> tooShort(10, 0);
    REQUIRE_THROWS_AS(unpackIndices(tooShort, 4, stratum::chunk::kBlocksPerSection), FormatError);
    REQUIRE_THROWS_WITH(unpackIndices(tooShort, 4, stratum::chunk::kBlocksPerSection),
                        Catch::Matchers::ContainsSubstring("need"));
}

TEST_CASE("a uniform section needs no data array", "[chunk]") {
    // What vanilla writes for a section of nothing but one block.
    const Chunk chunk = decode(buildChunkNbt(3, -5, {uniformSection(0, "minecraft:stone")}));

    CHECK(chunk.x() == 3);
    CHECK(chunk.z() == -5);
    CHECK(chunk.status() == "minecraft:full");
    REQUIRE(chunk.sections().size() == 1U);

    for (int y : {0, 7, 15}) {
        CAPTURE(y);
        const stratum::chunk::BlockState* state = chunk.blockAt(0, y, 0);
        REQUIRE(state != nullptr);
        CHECK(state->name == "minecraft:stone");
    }
}

TEST_CASE("blocks are addressed in YZX order", "[chunk]") {
    SectionSpec section;
    section.y = 0;
    section.palette = {"minecraft:air", "minecraft:stone", "minecraft:dirt"};
    section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    // One identifiable block, at a position that is wrong under any other
    // index order.
    const std::size_t index = (5U * 256U) + (3U * 16U) + 2U; // y=5, z=3, x=2
    section.blocks[index] = 1;
    section.blocks[index + 1U] = 2; // x=3

    const Chunk chunk = decode(buildChunkNbt(0, 0, {section}));
    CHECK(chunk.blockAt(2, 5, 3)->name == "minecraft:stone");
    CHECK(chunk.blockAt(3, 5, 3)->name == "minecraft:dirt");
    CHECK(chunk.blockAt(2, 5, 4)->name == "minecraft:air");
    CHECK(chunk.blockAt(2, 6, 3)->name == "minecraft:air");
}

TEST_CASE("sections below y=0 resolve, as they have since 1.18", "[chunk][landmine]") {
    // floorDiv, not `/`: truncation folds section -1 onto section 0 and the
    // world below bedrock reads back as the world above it.
    const Chunk chunk = decode(buildChunkNbt(0, 0,
                                             {uniformSection(-4, "minecraft:deepslate"),
                                              uniformSection(-1, "minecraft:stone"),
                                              uniformSection(0, "minecraft:dirt")}));

    CHECK(chunk.minY() == -64);
    CHECK(chunk.maxY() == 15);
    CHECK(chunk.blockAt(0, -64, 0)->name == "minecraft:deepslate");
    CHECK(chunk.blockAt(0, -49, 0)->name == "minecraft:deepslate");
    CHECK(chunk.blockAt(0, -16, 0)->name == "minecraft:stone");
    CHECK(chunk.blockAt(0, -1, 0)->name == "minecraft:stone");
    CHECK(chunk.blockAt(0, 0, 0)->name == "minecraft:dirt");
    CHECK(chunk.blockAt(0, -48, 0) == nullptr); // no section there
    CHECK(chunk.blockAt(0, 4096, 0) == nullptr);
}

TEST_CASE("block state properties are compared by meaning, not by order", "[chunk]") {
    stratum::chunk::BlockState left{"minecraft:oak_stairs", {{"facing", "north"}, {"half", "top"}}};
    stratum::chunk::BlockState right{"minecraft:oak_stairs",
                                     {{"half", "top"}, {"facing", "north"}}};
    std::sort(right.properties.begin(), right.properties.end());

    CHECK(left == right);
    CHECK(left.toString() == "minecraft:oak_stairs[facing=north,half=top]");
    CHECK(stratum::chunk::BlockState{"minecraft:stone", {}}.toString() == "minecraft:stone");
}

TEST_CASE("the highest non-air block is found, ignoring the air variants", "[chunk]") {
    SectionSpec section;
    section.y = 0;
    section.palette = {"minecraft:air", "minecraft:stone", "minecraft:cave_air"};
    section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    section.blocks[(4U * 256U)] = 1; // stone at y=4
    section.blocks[(9U * 256U)] = 2; // cave_air at y=9 must not count

    const Chunk chunk = decode(buildChunkNbt(0, 0, {section}));
    REQUIRE(chunk.highestNonAir(0, 0).has_value());
    CHECK(*chunk.highestNonAir(0, 0) == 4);
    CHECK_FALSE(chunk.highestNonAir(1, 0).has_value());
}

TEST_CASE("biomes decode from their own paletted container", "[chunk]") {
    SectionSpec section = uniformSection(0, "minecraft:stone");
    section.biomePalette = {"minecraft:plains", "minecraft:desert", "minecraft:ocean"};
    section.biomes.assign(stratum::chunk::kBiomesPerSection, 0);
    section.biomes[1] = 1;
    section.biomes[63] = 2;

    const Chunk chunk = decode(buildChunkNbt(0, 0, {section}));
    REQUIRE(chunk.sections().size() == 1U);
    const stratum::chunk::Section& decoded = chunk.sections().front();
    REQUIRE(decoded.biomes.size() == stratum::chunk::kBiomesPerSection);
    CHECK(decoded.biomePalette[decoded.biomes[0]] == "minecraft:plains");
    CHECK(decoded.biomePalette[decoded.biomes[1]] == "minecraft:desert");
    CHECK(decoded.biomePalette[decoded.biomes[63]] == "minecraft:ocean");
}

TEST_CASE("chunks that cannot be decoded are refused, not guessed at", "[chunk][malformed]") {
    SECTION("no coordinates") {
        stratum::test::NbtWriter writer;
        writer.named(stratum::nbt::TagType::Compound, "");
        writer.named(stratum::nbt::TagType::Int, "DataVersion").u32(4440);
        writer.end();
        REQUIRE_THROWS_WITH(decode(writer), Catch::Matchers::ContainsSubstring("xPos"));
    }

    SECTION("no sections list") {
        stratum::test::NbtWriter writer;
        writer.named(stratum::nbt::TagType::Compound, "");
        writer.named(stratum::nbt::TagType::Int, "xPos").u32(0);
        writer.named(stratum::nbt::TagType::Int, "zPos").u32(0);
        writer.end();
        REQUIRE_THROWS_WITH(decode(writer), Catch::Matchers::ContainsSubstring("sections"));
    }

    SECTION("an index outside the palette") {
        SectionSpec section;
        section.y = 0;
        section.palette = {"minecraft:air", "minecraft:stone"};
        section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
        section.blocks[0] = 9; // only 2 entries exist
        REQUIRE_THROWS_WITH(decode(buildChunkNbt(0, 0, {section})),
                            Catch::Matchers::ContainsSubstring("palette"));
    }

    SECTION("a multi-entry palette with no data array") {
        SectionSpec section;
        section.y = 0;
        section.palette = {"minecraft:air", "minecraft:stone"};
        // blocks left empty, so no data array is written
        REQUIRE_THROWS_WITH(decode(buildChunkNbt(0, 0, {section})),
                            Catch::Matchers::ContainsSubstring("no data array"));
    }
}

TEST_CASE("stored heightmaps are measured from yPos, not from the sections present",
          "[chunk][heightmap]") {
    // The discriminating case, which vanilla's own regions cannot provide:
    // a chunk whose lowest *present* section is not its yPos. Real chunks
    // write every section, so a decoder that inferred the origin from the
    // sections agrees with one that reads yPos on every golden region there
    // is — and would be wrong the moment it met a chunk like this one.
    std::vector<std::uint16_t> stored(256, 0);
    stored[0] = 65;                // y + 1 - minY, with minY = -64: y = 0.
    stored[1] = 1;                 // the lowest block there is: y = -64.
    stored[2] = 0;                 // nothing in this column at all.
    stored[(3U * 16U) + 5U] = 130; // y = 65, at column x=5, z=3.

    const stratum::test::NbtWriter writer = stratum::test::buildChunkNbt(
        0, 0, {stratum::test::uniformSection(0, "minecraft:stone")}, 4440,
        /*yPos=*/-4, {{"WORLD_SURFACE", stored}});

    const stratum::chunk::Chunk chunk =
        stratum::chunk::Chunk::decode(stratum::nbt::read(writer.bytes()).root);

    REQUIRE(chunk.lowestSection() == -4);
    const auto heights = chunk.heightmap(stratum::chunk::Heightmap::WorldSurface);
    REQUIRE(heights.has_value());
    REQUIRE(heights->size() == 256U);

    CHECK((*heights)[0] == 0);
    CHECK((*heights)[1] == -64);
    CHECK_FALSE((*heights)[2].has_value());
    // ZX order: column (x=5, z=3) is index 3*16 + 5, not 5*16 + 3.
    CHECK((*heights)[(3U * 16U) + 5U] == 65);
    CHECK_FALSE((*heights)[(5U * 16U) + 3U].has_value());

    // A map the chunk does not carry is absent, not empty.
    CHECK_FALSE(chunk.heightmap(stratum::chunk::Heightmap::OceanFloor).has_value());
}

TEST_CASE("a chunk with no heightmaps says so", "[chunk][heightmap]") {
    const stratum::test::NbtWriter writer =
        stratum::test::buildChunkNbt(0, 0, {stratum::test::uniformSection(0, "minecraft:stone")});
    const stratum::chunk::Chunk chunk =
        stratum::chunk::Chunk::decode(stratum::nbt::read(writer.bytes()).root);

    CHECK_FALSE(chunk.heightmap(stratum::chunk::Heightmap::WorldSurface).has_value());
    // yPos is optional in the format, and its absence must not be read as a
    // silent zero somewhere that matters.
    CHECK(chunk.lowestSection() == 0);
}
