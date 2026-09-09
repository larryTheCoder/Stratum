// Stratum — the density chain, measured without the aquifer step in the way.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// golden_terrain_test.cpp compares `final_density` against the OCEAN_FLOOR
// heightmaps of ordinary golden regions, and what it measures is dominated by
// something this build does not implement: vanilla's overworld runs aquifers,
// and an aquifer places a stone BARRIER between two bodies of water at
// different levels. Those are solid blocks no density function produced, so
// OCEAN_FLOOR reports them and the comparison blames the density.
//
// This test removes that. tools/analysis/aquifer-free-probe.sh regenerates the
// same world from vanilla's own overworld noise settings with one field
// changed — `aquifers_enabled: false` — so OCEAN_FLOOR really is where
// `final_density` last crossed zero, and the comparison measures this build's
// arithmetic.
//
// The difference the aquifer step makes, on seed -1 over the same 4096
// columns:
//
//              with aquifers      without
//   exact         96.167%         98.267%
//   within one    97.949%        100.000%
//   worst        50 blocks        1 block
//
// So what is left once aquifers are out of the way is small and uniform: no
// column is off by more than one block. That residual is REAL — at a
// disagreeing column the density in dispute is of order 1e-3, not 1e-15, so it
// is not a tie broken differently — but it is two orders of magnitude below
// what the aquifer gap was contributing.
//
// CORRECTED, not what an earlier pass of this comment said: it DOES correlate
// with the cell lattice, strongly — SPEC §11 records disagreements bucketed
// by y-offset-in-cell, and a since-corrected claim that offset 0 (the cell
// corner) was exactly clean turned out to be an artifact of this test's own
// sparse column sampling (every eighth) rather than a property of the
// formula. A full-column scan finds corner-level disagreements too, some of
// them THIS BUILD's own computation being wrong before any interpolation
// runs at all. See SPEC §11 and `tools/analysis/final-density-probe.sh`,
// which reads the server's real density at an arbitrary point directly.
//
// The fixture is Mojang-derived and never committed (SPEC §12). Without it
// this skips.
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

using stratum::density::Point;

constexpr std::int64_t kSeed = -1;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Comparison {
    std::size_t columns = 0;
    std::size_t exact = 0;
    std::size_t withinOne = 0;
    long long worst = 0;
};

} // namespace

TEST_CASE("the terrain chain, compared without aquifers in the way", "[conformance][terrain]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region =
        fixtures() / "probes" / "no-aquifer" / ("seed-" + std::to_string(kSeed)) / "r.0.0.mca";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_regular_file(region)) {
        SKIP("no aquifer-free probe at " << region << "; generate it with "
                                         << "tools/analysis/aquifer-free-probe.sh --accept-eula");
    }

    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), kSeed, stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                       .height = overworld.geometry.cellHeight()});
    const auto root = overworld.router.at(stratum::settings::RouterEntry::FinalDensity);

    const int minY = overworld.geometry.minY;
    const int height = overworld.geometry.height;
    const auto file = stratum::region::RegionFile::open(region);

    Comparison result;
    for (std::int32_t chunkZ = 0; chunkZ < 8; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 8; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            const auto floor = chunk.heightmap(stratum::chunk::Heightmap::OceanFloor);
            if (!floor.has_value()) {
                continue;
            }
            // Every eighth column, as golden_terrain_test.cpp samples, so the
            // two are measuring the same 256 places and their numbers can be
            // set beside each other.
            for (int localZ = 0; localZ < 16; localZ += 8) {
                for (int localX = 0; localX < 16; localX += 8) {
                    const auto stored = (*floor)[static_cast<std::size_t>((localZ * 16) + localX)];
                    if (!stored.has_value()) {
                        continue;
                    }
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;

                    int ours = minY - 1;
                    for (int y = minY + height - 1; y >= minY; --y) {
                        if (interpreter.evaluate(root, Point{.x = x, .y = y, .z = z}) > 0.0) {
                            ours = y;
                            break;
                        }
                    }
                    ++result.columns;
                    const long long difference = ours - *stored;
                    if (difference == 0) {
                        ++result.exact;
                    }
                    if (std::llabs(difference) <= 1) {
                        ++result.withinOne;
                    }
                    result.worst = std::max(result.worst, std::llabs(difference));
                }
            }
        }
    }

    REQUIRE(result.columns == 256U);

    // The claim this test exists to make: with the aquifer step out of the
    // way, NOTHING is off by more than a single block. Against the same
    // columns with aquifers on, the worst is 28.
    CHECK(result.worst <= 1);
    CHECK(result.withinOne == result.columns);

    // Pinned rather than thresholded, like its sibling. If this moves, the
    // density chain moved. 253 of 256 here against 249 with aquifers on, over
    // the same columns.
    CHECK(result.exact == 253U);
}

TEST_CASE("the terrain residual lives strictly between the cell's y boundaries",
          "[conformance][terrain]") {
    // The heightmap test above says 1.7% of columns are off by one block. This
    // one says WHERE, and it compares blocks rather than heights — the
    // heightmap only reports a column's topmost solid, so it cannot see that
    // the disagreements avoid one y offset entirely.
    //
    // The overworld's interpolation cell is 4 wide and 8 tall (size_horizontal
    // 1, size_vertical 2), and `min_y` is -64, so `(y + 64) % 8 == 0` is a cell
    // boundary — the one place `minecraft:interpolated` returns its argument
    // rather than a blend of two corners. Over the whole region that offset is
    // clean and every other offset is not, which is the opposite of what SPEC
    // recorded before this test existed.
    //
    // This is not an absence of close calls at the boundary. Over the wider
    // band y in [-60, 120] on these same chunks, offset 0 carries 5029
    // densities within 1e-2 of zero and 515 within 1e-3, against 5067/512 to
    // 5782/577 at the other seven — the same exposure. The sign simply never
    // comes out wrong there: 0 of 94208, where the other offsets contribute
    // between 12 and 37 each.
    //
    // Nor is it the cell height being wrong, which would produce the same
    // shape. Scored over the same blocks, a height of 8 disagrees on 148, and
    // 4, 16 and 2 disagree on 9512, 22232 and 11992.
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region =
        fixtures() / "probes" / "no-aquifer" / ("seed-" + std::to_string(kSeed)) / "r.0.0.mca";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_regular_file(region)) {
        SKIP("no aquifer-free probe at " << region);
    }

    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), kSeed, stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                       .height = overworld.geometry.cellHeight()});
    const auto root = overworld.router.at(stratum::settings::RouterEntry::FinalDensity);
    const int cellHeight = overworld.geometry.cellHeight();
    const int minY = overworld.geometry.minY;
    const auto file = stratum::region::RegionFile::open(region);

    // Every disagreement anywhere in the region sits in y 17..47, so this band
    // holds all of them while keeping the test to about fifty thousand
    // evaluations.
    constexpr int kLowY = 10;
    constexpr int kHighY = 60;
    long long onBoundary = 0;
    long long betweenBoundaries = 0;
    long long boundaryBlocks = 0;
    for (std::int32_t chunkZ = 0; chunkZ < 4; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 4; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (int y = kLowY; y <= kHighY; ++y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr) {
                            continue;
                        }
                        // What OCEAN_FLOOR counts: neither air nor fluid.
                        const std::string name = block->toString();
                        const bool serverSolid = name.find("air") == std::string::npos &&
                                                 name.find("water") == std::string::npos &&
                                                 name.find("lava") == std::string::npos;
                        const bool oursSolid =
                            interpreter.evaluate(root, Point{.x = (chunkX * 16) + localX,
                                                             .y = y,
                                                             .z = (chunkZ * 16) + localZ}) > 0.0;
                        const bool atBoundary = (y - minY) % cellHeight == 0;
                        if (atBoundary) {
                            ++boundaryBlocks;
                        }
                        if (serverSolid == oursSolid) {
                            continue;
                        }
                        if (atBoundary) {
                            ++onBoundary;
                        } else {
                            ++betweenBoundaries;
                        }
                    }
                }
            }
        }
    }

    // The test has to be exercising something, or the claim below is vacuous.
    REQUIRE(boundaryBlocks > 10000);
    CHECK(betweenBoundaries > 0);

    // The claim. If this ever becomes non-zero, the fault has moved out of the
    // interpolation and the analysis in SPEC §11 needs redoing.
    CHECK(onBoundary == 0);
}
