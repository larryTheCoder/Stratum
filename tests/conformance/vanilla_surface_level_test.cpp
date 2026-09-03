// Stratum — preliminary_surface_level, read off the vanilla server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `find_top_surface` sits at the root of the overworld's
// `preliminary_surface_level`, and the whole entry is column-invariant — it
// returns a y and nothing about it varies with height. So the datapack probe
// reads it directly: a dimension whose entire `final_density` is
//
//     K * flat_cache(<vanilla's preliminary_surface_level, verbatim>) + gradient
//
// puts the value it returns into the terrain height, where it can be read
// back out. tools/analysis/density-probe.sh builds it.
//
// This checks more than the one node. The entry reaches `invert`, which until
// now was the ONLY density function type vanilla uses that nothing in this
// project had ever compared against anything — all three of its uses are
// inside `preliminary_surface_level`, which was refused for want of
// `find_top_surface`. Implementing that made `invert` live, and this is where
// it is checked.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>

namespace {

// Must match the spec the probe was run with.
constexpr double kK = 0.35;
constexpr double kMinY = -64.0;
constexpr double kHeight = 384.0;
constexpr double kCentre = 96.0;
constexpr double kScale = 0.02;
constexpr std::int64_t kSeed = 42;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

} // namespace

TEST_CASE("the surface level the server itself computes is the one this build computes",
          "[conformance][surface]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region =
        fixtures() / "probes" / "psl" / "psl_overworld" / "r.0.0.mca";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_regular_file(region)) {
        SKIP("no preliminary_surface_level probe at "
             << region << "; generate it with tools/analysis/density-probe.sh");
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
    const auto root = overworld.router.at(stratum::settings::RouterEntry::PreliminarySurfaceLevel);
    REQUIRE_NOTHROW(interpreter.requireEvaluable(root));

    const auto file = stratum::region::RegionFile::open(region);

    // The probe reads the value to within half of one terrain block, which at
    // this scale is 0.375 of a y level. The entry returns a multiple of its
    // cell_height, eight, so that is ample to identify the answer exactly.
    constexpr double kResolution = (2.0 / (kHeight * kK)) / kScale;

    std::size_t columns = 0;
    std::size_t agreeing = 0;
    double worst = 0.0;

    for (std::int32_t chunkZ = 0; chunkZ < 8; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 8; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    // Cell corners only, and inside the forceloaded area.
                    if ((x % 4) != 0 || (z % 4) != 0 || x > 127 || z > 127) {
                        continue;
                    }
                    int surface = static_cast<int>(kMinY) - 1;
                    for (int y = static_cast<int>(kMinY + kHeight) - 1; y >= kMinY; --y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block != nullptr && block->name != "minecraft:air") {
                            surface = y;
                            break;
                        }
                    }
                    REQUIRE(surface >= kMinY);

                    const double gradient = 1.0 - (2.0 * ((surface + 0.5) - kMinY) / kHeight);
                    const double theirs = kCentre + ((-gradient / kK) / kScale);
                    const double ours =
                        interpreter.evaluate(root, stratum::density::Point{.x = x, .y = 0, .z = z});

                    ++columns;
                    const double error = std::abs(ours - theirs);
                    worst = std::max(worst, error);
                    if (error <= 0.5 * kResolution) {
                        ++agreeing;
                    }
                }
            }
        }
    }

    REQUIRE(columns == 1024U);
    // Every column, to the limit of what the probe can resolve.
    CHECK(agreeing == columns);
    CHECK(worst <= 0.5 * kResolution);
}
