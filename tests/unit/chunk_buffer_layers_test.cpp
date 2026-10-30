// Stratum — a ChunkBuffer's palette indices, a run of whole layers at a time.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ChunkBuffer::paletteIndicesOfLayers hands a caller reading a whole section
// (stratum::pmmp::encodeChunk) the section's indices at once, checked once
// rather than once a block. Held here to paletteIndexAt at every position of
// every run it can hand out, and to `at`'s refusal for a run that leaves the
// dimension at either end.
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

using Catch::Matchers::ContainsSubstring;
using stratum::settings::BlockState;
using stratum::terrain::ChunkBuffer;
using stratum::terrain::FillError;

namespace {

[[nodiscard]] BlockState named(const std::string& name) {
    return BlockState{.name = stratum::data::ResourceLocation::parse(name), .properties = {}};
}

} // namespace

TEST_CASE("a run of layers holds paletteIndexAt's index at every position of it",
          "[terrain][buffer]") {
    const stratum::settings::NoiseGeometry geometry{
        .minY = -32, .height = 64, .sizeHorizontal = 1, .sizeVertical = 2};
    ChunkBuffer buffer(geometry);
    const std::array<BlockState, 4> blocks = {named("minecraft:stone"), named("minecraft:dirt"),
                                              named("minecraft:water"), named("minecraft:sand")};
    // A pattern that tells x, y and z apart.
    for (std::int32_t y = -32; y < 32; ++y) {
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const auto pick = static_cast<std::size_t>((x * 7) + (z * 3) + (y * 5) + 64) % 5U;
                if (pick < blocks.size()) {
                    buffer.set(x, y, z, blocks.at(pick));
                }
            }
        }
    }
    for (const auto& [from, layers] : std::array<std::pair<std::int32_t, std::int32_t>, 5>{
             {{-32, 16}, {-16, 16}, {16, 16}, {-32, 64}, {5, 3}}}) {
        CAPTURE(from, layers);
        const std::span<const std::uint16_t> run = buffer.paletteIndicesOfLayers(from, layers);
        REQUIRE(run.size() == static_cast<std::size_t>(layers) * 256U);
        std::size_t wrong = 0;
        for (std::int32_t y = from; y < from + layers; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const std::size_t at = (((static_cast<std::size_t>(y - from) * 16U) +
                                             static_cast<std::size_t>(z)) *
                                            16U) +
                                           static_cast<std::size_t>(x);
                    wrong += run[at] == buffer.paletteIndexAt(x, y, z) ? 0U : 1U;
                }
            }
        }
        CHECK(wrong == 0U);
    }
    CHECK(buffer.paletteIndicesOfLayers(31, 1).size() == 256U);
    CHECK(buffer.paletteIndicesOfLayers(32, 0).empty());
}

TEST_CASE("a run of layers that leaves the dimension is refused as `at` refuses its y",
          "[terrain][buffer]") {
    const stratum::settings::NoiseGeometry geometry{
        .minY = -32, .height = 64, .sizeHorizontal = 1, .sizeVertical = 2};
    const ChunkBuffer buffer(geometry);
    CHECK_THROWS_AS(buffer.paletteIndicesOfLayers(-33, 16), FillError);
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(-33, 16),
                      ContainsSubstring("y -33 is outside this dimension, which runs -32 to 31"));
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(17, 16),
                      ContainsSubstring("y 32 is outside this dimension"));
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(0, 2147483647),
                      ContainsSubstring("y 32 is outside this dimension"));
    // A run that starts above the top is refused at its own first y, which
    // is the first outside, as `at` refuses it.
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(40, 1),
                      ContainsSubstring("y 40 is outside this dimension"));
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(33, 0),
                      ContainsSubstring("y 33 is outside this dimension"));
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(2147483647, 16),
                      ContainsSubstring("y 2147483647 is outside this dimension"));
    CHECK_THROWS_WITH(buffer.paletteIndicesOfLayers(0, -1), ContainsSubstring("-1 layers"));
}
