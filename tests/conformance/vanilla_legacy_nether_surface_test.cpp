// Stratum — the legacy Nether, surface rules and all, block for block against
// the blocks the vanilla server wrote.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// THE GOLDEN CONFIRMATION of spec/legacy-named-noise-spec.md Q1-Q4
// (IMPLEMENTER-RULING-01 §2: a claim enters lib/ only once a golden confirms
// it), and of the block-level biome zoom (biome/voronoi.hpp). Until that
// spec the Nether's surface rule could not run: it reads six named noises
// plus `minecraft:surface` and `minecraft:surface_secondary`, and how a
// legacy dimension seeds a NAME was unknown, so
// vanilla_legacy_nether_terrain_test.cpp compared only air / fluid / solid
// classes. This case compiles the Nether as a world does (CompiledDimension,
// with the vanilla `minecraft:nether` multi-noise biome source) and compares
// stored block NAMES.
//
// With the named noises alone, three worlds were exact and five missed 118
// to 1430 blocks each, every one a biome-specific block in a quart whose
// stored biome agreed, skewed toward the quart's edge: the surface rules
// read the ZOOMED biome, not the quart grid. With the zoom every block of
// every region agrees (SPEC §11, "The surface rules read the zoomed biome").
//
// The goldens are tools/fetch-vanilla's terrain-only worlds — carvers and
// features stripped — so between this build's output and the golden there
// is the density chain, the biome source and the surface rule, and nothing
// else. Eight region files: 0 and LONG_MIN, -1 and LONG_MAX share their
// noises (java.util.Random keeps 48 bits) but not their zoom, which hashes
// all 64 — and the goldens of each pair differ exactly where the zoom does.

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::world::BiomeSource;
using stratum::world::CompiledDimension;

namespace {

constexpr std::array<std::int64_t, 8> kSeeds = {0,
                                                1,
                                                -1,
                                                42,
                                                2891948927356891,
                                                -4172144997902289642,
                                                9223372036854775807,
                                                -9223372036854775807 - 1};

/// Chunks per axis of r.0.0 compared: a cost dial, not a coverage claim.
constexpr std::int32_t kChunksPerAxis = 8;

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

} // namespace

TEST_CASE("the legacy Nether's surface rules place the blocks the server wrote",
          "[conformance][surface][legacy][nether]") {
    if (!std::filesystem::is_directory(versionDir() / "worldgen" / "noise_settings") ||
        !std::filesystem::is_directory(versionDir() / "biome_parameters")) {
        SKIP("no worldgen or biome_parameters fixtures under " << versionDir()
                                                               << "; run tools/fetch-vanilla");
    }
    const Pack pack = Pack::open(versionDir() / "worldgen");
    const auto pipeline = stratum::freeze::resolve(pack, versionDir() / "biome_parameters");
    const auto nether = ResourceLocation::parse("minecraft:nether");

    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path region =
            versionDir() / "regions" / ("seed-" + std::to_string(seed)) / "nether" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no golden Nether region at " << region
                                               << "; generate it with tools/fetch-vanilla");
        }
        const auto dimension = CompiledDimension::compile(
            pipeline, nether, BiomeSource::parse("minecraft:nether"), seed);
        const auto file = stratum::region::RegionFile::open(region);
        const auto& geometry = dimension->geometry();

        std::size_t blocks = 0;
        std::size_t exact = 0;
        std::map<std::pair<std::string, std::string>, std::size_t> confusions;
        for (std::int32_t chunkZ = 0; chunkZ < kChunksPerAxis; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < kChunksPerAxis; ++chunkX) {
                REQUIRE(file.hasChunk(chunkX, chunkZ));
                const auto golden = stratum::chunk::Chunk::decode(
                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
                stratum::terrain::ChunkBuffer buffer(geometry);
                dimension->fillBlocks(chunkX, chunkZ, buffer);
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        for (std::int32_t y = geometry.minY; y < geometry.maxY(); ++y) {
                            const std::string ours = buffer.at(x, y, z).name.toString();
                            const auto* block = golden.blockAt(x, y, z);
                            const std::string theirs =
                                block != nullptr ? block->name : std::string("minecraft:air");
                            ++blocks;
                            if (ours == theirs) {
                                ++exact;
                            } else {
                                ++confusions[{ours, theirs}];
                            }
                        }
                    }
                }
            }
        }
        std::string listed;
        for (const auto& [pair, count] : confusions) {
            listed += "\n  ours " + pair.first + " / server " + pair.second + ": " +
                      std::to_string(count);
        }
        INFO("seed " << seed << ": " << exact << " of " << blocks << " blocks exact" << listed);
        CHECK(blocks == static_cast<std::size_t>(kChunksPerAxis) * kChunksPerAxis * 16U * 16U *
                            static_cast<std::size_t>(geometry.height));
        CHECK(exact == blocks);
    }
}
