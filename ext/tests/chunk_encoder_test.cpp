// Stratum — packing sub-chunks the way chunkutils2's fromData reads them.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free. The byte counts are chunkutils2 0.3.5's own
// (tests/PalettedBlockArray/get-expected-word-array-size.phpt), not
// recomputed from this encoder's formula, and every packed layer is decoded
// by a reader written from chunkutils2's description of the format rather
// than by inverting the encoder — so a shared misreading of the format would
// have to be made twice, independently, to pass.
#include <stratum/rng/java_random.hpp>

#include <stratum_pmmp/chunk_encoder.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
    // int rather than uint8_t members: MSVC warns (C4242, an error here) on
    // brace-initialising a narrower member from an int literal.
    struct WidthCase {
        int width;
        std::size_t bytes;
    };

    constexpr std::array<WidthCase, 9> kExpectedBytes = {{{0, 0},
                                                          {1, 512},
                                                          {2, 1024},
                                                          {3, 1640},
                                                          {4, 2048},
                                                          {5, 2732},
                                                          {6, 3280},
                                                          {8, 4096},
                                                          {16, 8192}}};
    for (const auto& [width, bytes] : kExpectedBytes) {
        CAPTURE(width);
        CHECK(wordCount(static_cast<std::uint8_t>(width)) * 4U == bytes);
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
    struct PaletteCase {
        std::uint32_t count;
        int width;
    };

    constexpr std::array<PaletteCase, 9> kCases = {
        {{2, 1}, {3, 2}, {4, 2}, {5, 3}, {9, 4}, {17, 5}, {33, 6}, {65, 8}, {257, 16}}};
    for (const auto& [count, width] : kCases) {
        CAPTURE(count);
        const auto values = distinctValues(count);
        const PalettedLayer layer = encodeLayer(values);
        CHECK(layer.bitsPerBlock == width);
        CHECK(layer.palette.size() == count);
        CHECK(layer.words.size() == wordCount(static_cast<std::uint8_t>(width)));
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

namespace {

/// The encoder as ext/src/chunk_encoder.cpp had it before the palette
/// builder and the word packer, copied verbatim (only the names changed):
/// the reference the current one is held to, byte for byte.
[[nodiscard]] std::uint8_t baseWidthFor(std::size_t paletteSize) {
    constexpr std::array<std::uint8_t, 8> kWidths = {1, 2, 3, 4, 5, 6, 8, 16};
    if (paletteSize == 1) {
        return 0;
    }
    for (const std::uint8_t width : kWidths) {
        if ((std::size_t{1} << width) >= paletteSize) {
            return width;
        }
    }
    throw stratum::pmmp::EncodeError("a sub-chunk palette of " + std::to_string(paletteSize) +
                                     " entries does not fit any width chunkutils2 accepts");
}

[[nodiscard]] PalettedLayer
baseEncodeLayer(const std::span<const std::uint32_t, kSubChunkVolume> values) {
    constexpr std::uint32_t kWordBits = 32;
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

    layer.bitsPerBlock = baseWidthFor(layer.palette.size());
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

/// A layer of exactly @p distinct values (each present at least once), laid
/// out as @p shape says: scattered at random (0), in runs of random length
/// (1), or sorted into long runs (2). Values are drawn from all of uint32,
/// 0 and UINT32_MAX among them, or, @p narrow, from multiples of 2^19,
/// which share every low bit a hash might take.
[[nodiscard]] std::array<std::uint32_t, kSubChunkVolume>
randomLayer(stratum::rng::JavaRandom& random, std::size_t distinct, int shape, bool narrow) {
    std::vector<std::uint32_t> palette;
    std::unordered_set<std::uint32_t> seen;
    while (palette.size() < distinct) {
        std::uint32_t value = 0;
        const std::int32_t pick = random.nextInt(16);
        if (narrow) {
            value = static_cast<std::uint32_t>(random.nextInt(8192)) << 19U;
        } else if (pick == 0) {
            value = 0;
        } else if (pick == 1) {
            value = 0xFFFF'FFFFU;
        } else {
            value = static_cast<std::uint32_t>(random.nextInt());
        }
        if (seen.insert(value).second) {
            palette.push_back(value);
        }
    }
    std::array<std::uint32_t, kSubChunkVolume> values{};
    for (std::uint32_t& value : values) {
        value = palette[static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(palette.size())))];
    }
    if (shape == 1) {
        std::size_t i = 0;
        while (i < kSubChunkVolume) {
            const std::uint32_t value = values[i];
            const std::size_t run = 1U + static_cast<std::size_t>(random.nextInt(40));
            for (std::size_t k = 0; k < run && i < kSubChunkVolume; ++k, ++i) {
                values[i] = value;
            }
        }
    } else if (shape == 2) {
        std::ranges::sort(values);
    }
    // Every palette value present at least once, at random positions, so
    // the layer holds exactly `distinct` values whatever the shape did.
    std::vector<std::size_t> positions(kSubChunkVolume);
    for (std::size_t i = 0; i < kSubChunkVolume; ++i) {
        positions[i] = i;
    }
    for (std::size_t i = 0; i < distinct; ++i) {
        const std::size_t j = i + static_cast<std::size_t>(random.nextInt(
                                      static_cast<std::int32_t>(kSubChunkVolume - i)));
        std::swap(positions[i], positions[j]);
        values[positions[i]] = palette[i];
    }
    return values;
}

} // namespace

TEST_CASE("the encoder packs every layer as the encoder it replaced did, byte for byte",
          "[pmmp][encode]") {
    // Every palette size at and either side of every width boundary, and a
    // spread between, in three layouts over two value ranges: 1 to 4096
    // distinct values.
    std::vector<std::size_t> counts;
    for (std::size_t edge = 1; edge <= kSubChunkVolume; edge *= 2) {
        counts.push_back(edge);
        if (edge < kSubChunkVolume) {
            counts.push_back(edge + 1);
        }
        if (edge > 2) {
            counts.push_back(edge - 1);
        }
    }
    for (const std::size_t extra :
         {std::size_t{5}, std::size_t{7}, std::size_t{12}, std::size_t{47}, std::size_t{100},
          std::size_t{300}, std::size_t{999}, std::size_t{3000}, std::size_t{4000}}) {
        counts.push_back(extra);
    }
    stratum::rng::JavaRandom random(20261029);
    std::size_t asked = 0;
    std::size_t differing = 0;
    std::size_t undecodable = 0;
    std::size_t wrongSize = 0;
    for (const std::size_t distinct : counts) {
        for (int shape = 0; shape < 3; ++shape) {
            for (const bool narrow : {false, true}) {
                const auto values = randomLayer(random, distinct, shape, narrow);
                const PalettedLayer ours = encodeLayer(values);
                ++asked;
                differing += ours == baseEncodeLayer(values) ? 0U : 1U;
                undecodable += decode(ours) == values ? 0U : 1U;
                wrongSize += ours.palette.size() == distinct ? 0U : 1U;
            }
        }
    }
    CAPTURE(asked);
    CHECK(differing == 0U);
    CHECK(undecodable == 0U);
    CHECK(wrongSize == 0U);
}

TEST_CASE("a quart layer packs as its 4096 positions do, byte for byte", "[pmmp][encode]") {
    // encodeQuartLayer against encodeLayer and the replaced encoder of the
    // expansion — each position holding its quart's value, quart
    // (qx, qy, qz) at (qx << 4) | (qz << 2) | qy — for 1 to 64 distinct
    // values, shuffled over the quarts so that the first-seen order is
    // tested where it differs from the order of the values.
    stratum::rng::JavaRandom random(64);
    std::size_t asked = 0;
    std::size_t differing = 0;
    for (std::size_t distinct = 1; distinct <= stratum::pmmp::kSubChunkQuarts; ++distinct) {
        for (int trial = 0; trial < 40; ++trial) {
            std::vector<std::uint32_t> palette;
            for (std::size_t i = 0; i < distinct; ++i) {
                palette.push_back(trial % 2 == 0 ? static_cast<std::uint32_t>(i * 37U)
                                                 : static_cast<std::uint32_t>(random.nextInt()));
            }
            std::array<std::uint32_t, stratum::pmmp::kSubChunkQuarts> quarts{};
            for (std::size_t q = 0; q < quarts.size(); ++q) {
                quarts[q] = q < distinct ? palette[q]
                                         : palette[static_cast<std::size_t>(random.nextInt(
                                               static_cast<std::int32_t>(distinct)))];
            }
            for (std::size_t q = quarts.size() - 1; q > 0; --q) {
                std::swap(quarts[q], quarts[static_cast<std::size_t>(
                                         random.nextInt(static_cast<std::int32_t>(q + 1)))]);
            }
            std::array<std::uint32_t, kSubChunkVolume> values{};
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y < 16; ++y) {
                        const std::size_t quart = (static_cast<std::size_t>(x / 4) << 4U) |
                                                  (static_cast<std::size_t>(z / 4) << 2U) |
                                                  static_cast<std::size_t>(y / 4);
                        values[stratum::pmmp::subChunkIndex(x, y, z)] = quarts[quart];
                    }
                }
            }
            const PalettedLayer ours = stratum::pmmp::encodeQuartLayer(quarts);
            ++asked;
            differing += ours == encodeLayer(values) && ours == baseEncodeLayer(values) ? 0U : 1U;
        }
    }
    CAPTURE(asked);
    CHECK(differing == 0U);
}
