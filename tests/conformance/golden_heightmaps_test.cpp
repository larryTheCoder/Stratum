// Stratum — vanilla's own stored heightmaps, against our decoded blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// This is the first thing in the project to check one part of vanilla's
// output against another part of it, and it exists because the heightmaps
// are the oracle terrain will be compared against. A surface height is the
// only thing in a golden region that a density function can be judged by
// without first getting aquifers, surface rules and block mapping right
// (SPEC §7) — so it had better be read correctly before anything leans on
// it.
//
// The check is genuinely two-sided: vanilla wrote WORLD_SURFACE, and this
// repository's chunk decoder computes the same quantity from the blocks. The
// two share no code. Agreement means both the packing and the offset are
// right; disagreement would mean one of them is, and the test says which
// column.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.

#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using stratum::chunk::Chunk;
using stratum::chunk::Heightmap;

[[nodiscard]] std::vector<std::filesystem::path> findGoldenRegions() {
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    std::vector<std::filesystem::path> regions;
    if (!std::filesystem::is_directory(root)) {
        return regions;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mca" &&
            entry.path().parent_path().parent_path().filename().string().starts_with("seed-")) {
            regions.push_back(entry.path());
        }
    }
    std::ranges::sort(regions);
    return regions;
}

/// One region per dimension. The heightmap format does not vary with the
/// seed — it is the same packing and the same origin whatever the world
/// looks like — so walking all twenty-four regions costs eight times as long
/// to exercise the same code. What does vary is the dimension: the Nether
/// and the End declare yPos 0 where the overworld declares -4, and that is
/// the number the whole decode hangs off.
[[nodiscard]] std::vector<std::filesystem::path> oneRegionPerDimension() {
    std::vector<std::filesystem::path> chosen;
    for (const std::filesystem::path& region : findGoldenRegions()) {
        const std::string dimension = region.parent_path().filename().string();
        const bool seen = std::ranges::any_of(chosen, [&dimension](const auto& kept) {
            return kept.parent_path().filename().string() == dimension;
        });
        if (!seen) {
            chosen.push_back(region);
        }
    }
    return chosen;
}

} // namespace

TEST_CASE("vanilla's stored heightmaps agree with the blocks beneath them",
          "[conformance][heightmap]") {
    const std::vector<std::filesystem::path> regions = oneRegionPerDimension();
    if (regions.empty()) {
        SKIP("no golden regions under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla --generate-regions --accept-eula");
    }

    std::size_t columns = 0;
    std::size_t chunks = 0;
    std::size_t emptyColumns = 0;

    for (const std::filesystem::path& path : regions) {
        const auto region = stratum::region::RegionFile::open(path);
        CAPTURE(path.string());

        for (std::int32_t chunkZ = 0; chunkZ < stratum::region::kChunksPerAxis; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < stratum::region::kChunksPerAxis; ++chunkX) {
                if (!region.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto document = stratum::nbt::read(region.readChunk(chunkX, chunkZ));
                const Chunk chunk = Chunk::decode(document.root);

                const auto surface = chunk.heightmap(Heightmap::WorldSurface);
                REQUIRE(surface.has_value());
                REQUIRE(surface->size() == 256U);
                ++chunks;

                for (int z = 0; z < stratum::chunk::kSectionSize; ++z) {
                    for (int x = 0; x < stratum::chunk::kSectionSize; ++x) {
                        // ZX order, which is the one thing about this format
                        // that a transposition would hide in a symmetric
                        // world and not in a real one.
                        const auto stored = (*surface)[(static_cast<std::size_t>(z) * 16U) +
                                                       static_cast<std::size_t>(x)];
                        const auto computed = chunk.highestNonAir(x, z);
                        ++columns;
                        if (!computed.has_value()) {
                            ++emptyColumns;
                        }
                        CAPTURE(chunkX, chunkZ, x, z);
                        CHECK(stored == computed);
                    }
                }
            }
        }
    }

    // Three regions of 32x32 chunks, minus whatever the server did not
    // generate — and every column of every one of them.
    CHECK(chunks > 300U);
    CHECK(columns == chunks * 256U);
    // The End is mostly void, so some columns genuinely hold nothing — and
    // if none did, the nullopt path above would never have been exercised.
    CHECK(emptyColumns > 0U);
}

TEST_CASE("the ocean floor sits at or below the world surface", "[conformance][heightmap]") {
    // Every region here, but only a corner of each: what this checks is a
    // relation between two stored maps, and a seed that broke it would break
    // it in its first chunks as readily as its last.
    const std::vector<std::filesystem::path> regions = findGoldenRegions();
    if (regions.empty()) {
        SKIP("no golden regions under " << STRATUM_FIXTURES_DIR);
    }
    CHECK(regions.size() == 24U);

    // OCEAN_FLOOR is the map terrain will actually be compared against, so
    // it gets its own check rather than riding on WORLD_SURFACE's. There is
    // no second source for it — nothing here recomputes "highest non-fluid"
    // — so what is checked is the relation between the two, which is
    // enough to catch a wrong offset or a swapped pair of maps.
    std::size_t belowSurface = 0;
    std::size_t equalToSurface = 0;

    for (const std::filesystem::path& path : regions) {
        const auto region = stratum::region::RegionFile::open(path);
        for (std::int32_t chunkZ = 0; chunkZ < 4; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 4; ++chunkX) {
                if (!region.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto document = stratum::nbt::read(region.readChunk(chunkX, chunkZ));
                const Chunk chunk = Chunk::decode(document.root);
                const auto floor = chunk.heightmap(Heightmap::OceanFloor);
                const auto surface = chunk.heightmap(Heightmap::WorldSurface);
                REQUIRE(floor.has_value());
                REQUIRE(surface.has_value());

                for (std::size_t i = 0; i < floor->size(); ++i) {
                    const auto below = (*floor)[i];
                    const auto above = (*surface)[i];
                    CAPTURE(path.string(), chunkX, chunkZ, i);
                    if (!below.has_value() || !above.has_value()) {
                        continue;
                    }
                    CHECK(*below <= *above);
                    if (*below < *above) {
                        ++belowSurface;
                    } else {
                        ++equalToSurface;
                    }
                    // Every height must land inside the world the chunk
                    // declares. An offset that was wrong by a section would
                    // still be ordered correctly and would fail here.
                    CHECK(*below >= chunk.minY());
                    CHECK(*above <= chunk.maxY());
                }
            }
        }
    }

    // Both cases occur: dry land puts them equal, water puts the floor
    // below. If only one did, the comparison above would be vacuous.
    CHECK(belowSurface > 0U);
    CHECK(equalToSurface > 0U);
}
