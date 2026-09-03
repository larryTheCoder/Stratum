// Stratum — the whole terrain chain against vanilla's own recorded heights.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// With old_blended_noise and weird_scaled_sampler settled, the overworld's
// `final_density` evaluates end to end for the first time — climate, splines,
// the blended noise, the cell lattice, and the caves that reach terrain
// through `min`s nested deep inside it. This is the test that says how close
// that is to vanilla, and the answer is CLOSE BUT NOT RIGHT.
//
// A chunk's stored OCEAN_FLOOR heightmap is the highest block that is neither
// air nor fluid, which is where `final_density` last crossed zero — the same
// quantity deepslate was validated against (SPEC §7). So walking a column from
// the top and taking the highest y with a positive density must reproduce it.
//
// It does not, quite. The numbers below are pinned rather than thresholded
// because they are a statement about where this build stands, and they must
// move deliberately:
//
//   seed 42: 254 of 256 columns exact, all within one block
//   seed -1: 249 of 256 exact, 252 within one block, worst 28 blocks
//
// The residual is REAL, not rounding. At a disagreeing column the density at
// the block in dispute is of order 1e-4 to 1e-2, not 1e-15, so it is not a
// tie being broken differently — something in the chain is slightly wrong, and
// on seed -1 badly wrong on a few columns. SPEC §11 records it as the open
// question it is. Nothing here should be read as "terrain works".
//
// Sampling: every eighth column over 8x8 chunks, so 256 columns spread across
// 128x128 blocks rather than packed into one chunk. That matters — a single
// chunk of seed 42 comes out 256 of 256 and would have hidden the residual
// entirely. Full coverage costs six minutes a seed, which is why this samples;
// the wider runs are in SPEC.
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

constexpr int kChunks = 8;
constexpr int kStride = 8;

struct Comparison {
    std::size_t columns = 0;
    std::size_t exact = 0;
    std::size_t withinOne = 0;
    long long worst = 0;
};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] Comparison compare(std::int64_t seed) {
    using namespace stratum;
    const auto pack = data::Pack::open(fixtures() / "worldgen");
    const auto loaded = settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(data::ResourceLocation::parse("minecraft:overworld"));

    const auto noises = density::NoiseRegistry::create(pack, loaded.graph.referencedNoises(), seed,
                                                       density::RandomSource::Xoroshiro);
    const density::Interpreter interpreter(
        loaded.graph, noises,
        density::CellGeometry{.width = overworld.geometry.cellWidth(),
                              .height = overworld.geometry.cellHeight()});
    const density::NodeIndex root = overworld.router.at(settings::RouterEntry::FinalDensity);
    interpreter.requireEvaluable(root);

    const auto region = region::RegionFile::open(
        fixtures() / "regions" / ("seed-" + std::to_string(seed)) / "overworld" / "r.0.0.mca");
    const int minY = overworld.geometry.minY;
    const int height = overworld.geometry.height;

    Comparison result;
    for (std::int32_t chunkZ = 0; chunkZ < kChunks; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < kChunks; ++chunkX) {
            if (!region.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const chunk::Chunk decoded =
                chunk::Chunk::decode(nbt::read(region.readChunk(chunkX, chunkZ)).root);
            const auto floor = decoded.heightmap(chunk::Heightmap::OceanFloor);
            if (!floor.has_value()) {
                continue;
            }
            for (int localZ = 0; localZ < 16; localZ += kStride) {
                for (int localX = 0; localX < 16; localX += kStride) {
                    const auto stored = (*floor)[static_cast<std::size_t>((localZ * 16) + localX)];
                    if (!stored.has_value()) {
                        continue;
                    }
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    int ours = minY - 1;
                    for (int y = minY + height - 1; y >= minY; --y) {
                        if (interpreter.evaluate(root, density::Point{.x = x, .y = y, .z = z}) >
                            0.0) {
                            ours = y;
                            break;
                        }
                    }
                    ++result.columns;
                    const long long diff = ours - *stored;
                    if (diff == 0) {
                        ++result.exact;
                    }
                    if (std::llabs(diff) <= 1) {
                        ++result.withinOne;
                    }
                    result.worst = std::max(result.worst, std::llabs(diff));
                }
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("the terrain chain runs end to end, and is close but not right",
          "[conformance][terrain]") {
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(fixtures() / "regions")) {
        SKIP("no extracted worldgen tree or golden regions under " << STRATUM_FIXTURES_DIR);
    }

    SECTION("seed 42") {
        const Comparison result = compare(42);
        REQUIRE(result.columns == 256U);
        // Two columns out, both by exactly one block.
        CHECK(result.exact == 254U);
        CHECK(result.withinOne == 256U);
        CHECK(result.worst == 1);
    }

    SECTION("seed -1") {
        const std::filesystem::path region = fixtures() / "regions" / "seed--1";
        if (!std::filesystem::is_directory(region)) {
            SKIP("no golden region for seed -1");
        }
        const Comparison result = compare(-1);
        REQUIRE(result.columns == 256U);
        // Worse, and worse in kind: 28 blocks is not a boundary being resolved
        // differently, it is a column whose terrain this build gets wrong.
        // This is the number to watch when the residual is chased.
        CHECK(result.exact == 249U);
        CHECK(result.withinOne == 252U);
        CHECK(result.worst == 28);
    }
}
