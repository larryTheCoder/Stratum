// Stratum — a real vanilla chunk, packed for PocketMine-MP and read back.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite checks the packing on synthetic sub-chunks. This packs real
// overworld chunks compiled from a thawed vanilla blob and decodes every
// sub-chunk with a reader written from chunkutils2's description of the
// format: every block must decode to the Java state id of what the dimension
// placed there, every biome to its quart's Bedrock id, and an all-air
// sub-chunk must carry no block layer at all.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.
#include <stratum/data/pack.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <stratum_pmmp/chunk_encoder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <vector>

using stratum::data::ResourceLocation;
using stratum::pmmp::kSubChunkVolume;
using stratum::pmmp::PalettedLayer;

namespace {

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] std::uint32_t decodeAt(const PalettedLayer& layer, std::size_t i) {
    if (layer.bitsPerBlock == 0) {
        return layer.palette.at(0);
    }
    const std::size_t perWord = 32U / layer.bitsPerBlock;
    const std::uint32_t mask = (std::uint32_t{1} << layer.bitsPerBlock) - 1U;
    const std::uint32_t offset = (layer.words.at(i / perWord) >>
                                  static_cast<std::uint32_t>((i % perWord) * layer.bitsPerBlock)) &
                                 mask;
    return layer.palette.at(offset);
}

void checkShape(const PalettedLayer& layer) {
    CHECK(layer.words.size() == stratum::pmmp::wordCount(layer.bitsPerBlock));
    CHECK_FALSE(layer.palette.empty());
    CHECK(layer.palette.size() <= (std::size_t{1} << layer.bitsPerBlock));
}

} // namespace

TEST_CASE("a real overworld chunk packs into sub-chunks that read back exactly",
          "[conformance][pmmp]") {
    if (!std::filesystem::is_directory(versionDir() / "worldgen" / "noise_settings") ||
        !std::filesystem::is_directory(versionDir() / "biome_parameters")) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    const auto overworld = ResourceLocation::parse("minecraft:overworld");
    const stratum::data::Pack pack = stratum::data::Pack::open(versionDir() / "worldgen");
    const auto dimension = stratum::world::CompiledDimension::compile(
        stratum::freeze::read(stratum::freeze::write(
            stratum::freeze::resolve(pack, versionDir() / "biome_parameters"))),
        overworld, overworld, 20260915);
    const stratum::settings::NoiseGeometry& geometry = dimension->geometry();

    for (const auto& [chunkX, chunkZ] :
         std::array<std::pair<std::int32_t, std::int32_t>, 2>{{{0, 0}, {-3, 2}}}) {
        CAPTURE(chunkX, chunkZ);
        const auto encoded = stratum::pmmp::encodeChunk(*dimension, chunkX, chunkZ);

        stratum::terrain::ChunkBuffer blocks(geometry);
        dimension->fillBlocks(chunkX, chunkZ, blocks);
        std::vector<const ResourceLocation*> biomes(16U *
                                                    static_cast<std::size_t>(geometry.height / 4));
        dimension->fillBiomes(chunkX, chunkZ, biomes);

        REQUIRE(encoded.size() == 24U);
        std::size_t emptySections = 0;
        std::size_t packedSections = 0;
        for (std::size_t section = 0; section < encoded.size(); ++section) {
            const auto& sub = encoded[section];
            CAPTURE(sub.index);
            CHECK(sub.index == stratum::pmmp::kMinSubChunk + static_cast<std::int32_t>(section));
            const std::int32_t sectionMinY = sub.index * 16;
            checkShape(sub.biomes);
            if (sub.blocks.has_value()) {
                checkShape(*sub.blocks);
                packedSections += sub.blocks->bitsPerBlock > 0 ? 1U : 0U;
            } else {
                ++emptySections;
            }

            std::size_t blocksDiffering = 0;
            std::size_t biomesDiffering = 0;
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y < 16; ++y) {
                        const std::size_t at = stratum::pmmp::subChunkIndex(x, y, z);
                        const auto& state = blocks.at(x, sectionMinY + y, z);
                        if (sub.blocks.has_value()) {
                            blocksDiffering += decodeAt(*sub.blocks, at) ==
                                                       stratum::mapping::javaBlockStateId(state)
                                                   ? 0U
                                                   : 1U;
                        } else {
                            blocksDiffering += state == stratum::settings::BlockState{} ? 0U : 1U;
                        }
                        const auto quartY = static_cast<std::size_t>(
                            stratum::javamath::floorDiv(sectionMinY + y - geometry.minY, 4));
                        const std::size_t quart =
                            (quartY * 4U + static_cast<std::size_t>(z / 4)) * 4U +
                            static_cast<std::size_t>(x / 4);
                        const auto expected = stratum::mapping::bedrockBiomeId(*biomes.at(quart));
                        biomesDiffering +=
                            expected.has_value() && decodeAt(sub.biomes, at) ==
                                                        static_cast<std::uint32_t>(*expected)
                                ? 0U
                                : 1U;
                    }
                }
            }
            CHECK(blocksDiffering == 0U);
            CHECK(biomesDiffering == 0U);
        }
        // Not vacuous: sky above the terrain is empty, and the terrain below
        // needed real packing.
        CHECK(emptySections > 0U);
        CHECK(packedSections > 0U);
    }
}
