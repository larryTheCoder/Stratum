// Stratum — packing sub-chunks the way chunkutils2's fromData reads them.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free. The byte counts are chunkutils2 0.3.5's own
// (tests/PalettedBlockArray/get-expected-word-array-size.phpt), not
// recomputed from this encoder's formula, and every packed layer is decoded
// by a reader written from chunkutils2's description of the format rather
// than by inverting the encoder — so a shared misreading of the format would
// have to be made twice, independently, to pass.
#include <stratum_pmmp/chunk_encoder.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <map>

using Catch::Matchers::ContainsSubstring;
using stratum::pmmp::encodeLayer;
using stratum::pmmp::kSubChunkVolume;
using stratum::pmmp::PalettedLayer;
using stratum::pmmp::wordCount;

namespace {

/// chunkutils2's reading: 32 / width entries per uint32 word, entry i at
/// word i / perWord, shifted left by (i % perWord) * width from the low bit.
[[nodiscard]] std::array<std::uint32_t, kSubChunkVolume> decode(const PalettedLayer& layer) {
    std::array<std::uint32_t, kSubChunkVolume> values{};
    for (std::size_t i = 0; i < kSubChunkVolume; ++i) {
        std::uint32_t offset = 0;
        if (layer.bitsPerBlock != 0) {
            const std::size_t perWord = 32U / layer.bitsPerBlock;
            const std::uint32_t mask = (std::uint32_t{1} << layer.bitsPerBlock) - 1U;
            offset = (layer.words.at(i / perWord) >>
                      static_cast<std::uint32_t>((i % perWord) * layer.bitsPerBlock)) &
                     mask;
        }
        values[i] = layer.palette.at(offset);
    }
    return values;
}

[[nodiscard]] std::array<std::uint32_t, kSubChunkVolume> distinctValues(std::uint32_t count) {
    std::array<std::uint32_t, kSubChunkVolume> values{};
    for (std::size_t i = 0; i < kSubChunkVolume; ++i) {
        // Scattered, not in runs, so packing across word boundaries is
        // exercised at every width.
        values[i] = 1000U + static_cast<std::uint32_t>((i * 7919U) % count);
    }
    return values;
}

} // namespace

TEST_CASE("word counts are exactly the byte lengths chunkutils2 requires", "[pmmp]") {
    const std::map<std::uint8_t, std::size_t> expectedBytes = {{0, 0},    {1, 512},  {2, 1024},
                                                               {3, 1640}, {4, 2048}, {5, 2732},
                                                               {6, 3280}, {8, 4096}, {16, 8192}};
    for (const auto& [width, bytes] : expectedBytes) {
        CAPTURE(static_cast<int>(width));
        CHECK(wordCount(width) * 4U == bytes);
    }
    // chunkutils2 refuses these widths; so does the encoder.
    CHECK_THROWS_WITH(wordCount(7), ContainsSubstring("7 bits per block"));
    CHECK_THROWS_WITH(wordCount(9), ContainsSubstring("9 bits per block"));
}

TEST_CASE("a uniform sub-chunk packs to width zero with no words", "[pmmp]") {
    std::array<std::uint32_t, kSubChunkVolume> values{};
    values.fill(42);
    const PalettedLayer layer = encodeLayer(values);
    CHECK(layer.bitsPerBlock == 0);
    CHECK(layer.words.empty());
    CHECK(layer.palette == std::vector<std::uint32_t>{42});
    CHECK(decode(layer) == values);
}

TEST_CASE("each palette size gets the smallest width chunkutils2 accepts", "[pmmp]") {
    // 65 distinct values need 7 bits, which chunkutils2 does not accept: 8.
    const std::array<std::pair<std::uint32_t, std::uint8_t>, 9> cases = {
        {{2, 1}, {3, 2}, {4, 2}, {5, 3}, {9, 4}, {17, 5}, {33, 6}, {65, 8}, {257, 16}}};
    for (const auto& [count, width] : cases) {
        CAPTURE(count);
        const auto values = distinctValues(count);
        const PalettedLayer layer = encodeLayer(values);
        CHECK(layer.bitsPerBlock == width);
        CHECK(layer.palette.size() == count);
        CHECK(layer.words.size() == wordCount(width));
        CHECK(layer.palette.size() <= (std::size_t{1} << layer.bitsPerBlock));
        CHECK(decode(layer) == values);
    }
}

TEST_CASE("the palette is deduplicated in first-seen order", "[pmmp]") {
    std::array<std::uint32_t, kSubChunkVolume> values{};
    values.fill(7);
    values[0] = 9;
    values[1] = 7;
    values[4095] = 3;
    const PalettedLayer layer = encodeLayer(values);
    CHECK(layer.palette == std::vector<std::uint32_t>{9, 7, 3});
    CHECK(decode(layer) == values);
}

TEST_CASE("sub-chunk positions are numbered XZY", "[pmmp]") {
    CHECK(stratum::pmmp::subChunkIndex(0, 0, 0) == 0U);
    CHECK(stratum::pmmp::subChunkIndex(0, 1, 0) == 1U);
    CHECK(stratum::pmmp::subChunkIndex(0, 0, 1) == 16U);
    CHECK(stratum::pmmp::subChunkIndex(1, 0, 0) == 256U);
    CHECK(stratum::pmmp::subChunkIndex(15, 15, 15) == 4095U);
}
