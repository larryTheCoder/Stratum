// Stratum — golden region files, read back with our own tools.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// These regions were written by the vanilla server (tools/fetch-vanilla
// --generate-regions). Reading them is the first end-to-end check of the
// container reader, the NBT reader and the chunk decoder against Mojang's
// actual output rather than against fixtures we framed ourselves.
//
// The files are Mojang-derived and never committed (SPEC §12); without them
// this suite SKIPs, loudly, naming the command that produces them.

#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using stratum::chunk::Chunk;
using stratum::region::RegionFile;

struct GoldenRegion {
    std::filesystem::path path;
    std::string dimension;
    std::int32_t regionX = 0;
    std::int32_t regionZ = 0;
};

/// Parses `.../regions/seed-<n>/<dimension>/r.<x>.<z>.mca`.
///
/// Scoped to the collected goldens on purpose. `.mca` is a container format,
/// not a terrain format: a world directory also holds `poi/` and `entities/`
/// region files, whose chunk payloads are a different shape entirely and
/// carry no xPos/zPos. The container reader handles all three; only terrain
/// belongs to the chunk decoder.
[[nodiscard]] bool isCollectedGolden(const std::filesystem::path& path) {
    for (const std::filesystem::path& part : path) {
        if (part == "regions") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<GoldenRegion> findGoldenRegions() {
    std::vector<GoldenRegion> regions;
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return regions;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mca") {
            continue;
        }
        if (!isCollectedGolden(entry.path())) {
            continue;
        }
        const std::string stem = entry.path().stem().string(); // r.0.0
        const std::size_t firstDot = stem.find('.');
        const std::size_t lastDot = stem.rfind('.');
        if (stem.rfind("r.", 0) != 0 || firstDot == lastDot) {
            continue;
        }

        GoldenRegion region;
        region.path = entry.path();
        region.dimension = entry.path().parent_path().filename().string();
        region.regionX = std::stoi(stem.substr(firstDot + 1, lastDot - firstDot - 1));
        region.regionZ = std::stoi(stem.substr(lastDot + 1));
        regions.push_back(region);
    }

    std::sort(regions.begin(), regions.end(),
              [](const GoldenRegion& lhs, const GoldenRegion& rhs) { return lhs.path < rhs.path; });
    return regions;
}

/// The build height each dimension's noise settings declare.
struct HeightRange {
    int minY;
    int maxY;
};

[[nodiscard]] HeightRange expectedHeight(const std::string& dimension) {
    if (dimension == "overworld") {
        return {-64, 319};
    }
    return {0, 255}; // nether and end
}

} // namespace

TEST_CASE("golden regions read back correctly with our own tools", "[conformance][region][chunk]") {
    const std::vector<GoldenRegion> regions = findGoldenRegions();
    if (regions.empty()) {
        SKIP("no golden region files under "
             << STRATUM_FIXTURES_DIR
             << " — they are Mojang-derived and never committed (SPEC §12). Generate them "
                "with: tools/fetch-vanilla --generate-regions --accept-eula");
    }

    std::size_t totalChunks = 0;

    for (const GoldenRegion& region : regions) {
        CAPTURE(region.path.string(), region.dimension);
        const RegionFile file = RegionFile::open(region.path);

        // Generated with a margin, so every chunk of the region itself must
        // have reached full status. A partial chunk would mean the margin is
        // too small and the goldens are not comparable.
        CHECK(file.chunkCount() > 0U);

        const HeightRange height = expectedHeight(region.dimension);

        for (std::int32_t localZ = 0; localZ < stratum::region::kChunksPerAxis; ++localZ) {
            for (std::int32_t localX = 0; localX < stratum::region::kChunksPerAxis; ++localX) {
                if (!file.hasChunk(localX, localZ)) {
                    continue;
                }
                CAPTURE(localX, localZ);

                const stratum::nbt::Document document =
                    stratum::nbt::read(file.readChunk(localX, localZ));
                CHECK(document.rootName.empty());

                const Chunk chunk = Chunk::decode(document.root);
                ++totalChunks;

                // Where vanilla says the chunk is must agree with the slot it
                // was found in — the real-data check on indexFor's floorMod.
                CHECK(chunk.x() == (region.regionX * stratum::region::kChunksPerAxis) + localX);
                CHECK(chunk.z() == (region.regionZ * stratum::region::kChunksPerAxis) + localZ);

                CHECK(chunk.status() == "minecraft:full");
                CHECK(chunk.dataVersion() > 0);
                CHECK(chunk.minY() == height.minY);
                CHECK(chunk.maxY() == height.maxY);

                REQUIRE_FALSE(chunk.sections().empty());
                for (const stratum::chunk::Section& section : chunk.sections()) {
                    REQUIRE_FALSE(section.palette.empty());
                    CHECK(section.blocks.size() == stratum::chunk::kBlocksPerSection);
                    for (const stratum::chunk::BlockState& state : section.palette) {
                        CHECK(state.name.rfind("minecraft:", 0) == 0U);
                    }
                }

                // Bedrock at the very bottom of the overworld is a cheap
                // sanity check that y addressing is not inverted.
                if (region.dimension == "overworld") {
                    const stratum::chunk::BlockState* floor = chunk.blockAt(0, -64, 0);
                    REQUIRE(floor != nullptr);
                    CHECK(floor->name == "minecraft:bedrock");
                }
            }
        }
    }

    WARN("read " << totalChunks << " chunks from " << regions.size()
                 << " vanilla-written region file(s)");
    CHECK(totalChunks > 0U);
}
