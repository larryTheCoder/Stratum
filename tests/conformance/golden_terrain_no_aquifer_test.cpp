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
// what the aquifer gap was contributing, and it does not correlate strongly
// with the cell lattice, which is the first thing it would if the
// interpolation were wrong.
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
