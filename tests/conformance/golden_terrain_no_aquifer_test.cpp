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
// columns, AS MEASURED WHILE THE RESIDUAL BELOW WAS STILL OPEN:
//
//              with aquifers      without (then)      without (now)
//   exact         96.167%         98.267%              100.000%
//   within one    97.949%        100.000%              100.000%
//   worst        50 blocks        1 block               0 blocks
//
// RESOLVED. What was left once aquifers were out of the way traced, after
// several wrong turns recorded here and in SPEC §11, to one missing epsilon
// in `PerlinNoise::sample`'s fold — `old_blended_noise`'s Modern reading,
// which `base_3d_noise` is. A clean-room spec (`spec/blended-noise-spec.md`
// Q2.2/Q2.3) named `⌊clamp_source/d + 1e-7⌋ · d`; this build had the floor
// without the epsilon. Confirmed independently against the server via
// `tools/analysis/final-density-probe.sh`, which bisects vanilla's own
// density at an arbitrary point directly rather than trusting its sign:
// every corner the epsilon moved now lands inside the server's own bisected
// bracket, and the corners it does not touch are bit-for-bit unmoved. An
// exhaustive block-level rescan of this same fixture went from 322
// disagreements to 0.
//
// THE WRONG TURN worth keeping the record of: this comment used to say the
// residual "does not correlate strongly with the cell lattice", then
// corrected itself to say the opposite — that CELL CORNERS themselves were
// wrong, not just the interpolation between them — before the epsilon was
// found. Both readings were of the same underlying bug seen from different
// angles; neither was the mechanism. See SPEC §11 for the full chase.
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
    // density chain moved. EXACT, now — every one of these 256 columns,
    // since the fold's epsilon landed (SPEC §11). Before it: 253 of 256.
    CHECK(result.exact == result.columns);
}

TEST_CASE("the terrain chain agrees with the server at every block, not just every heightmap",
          "[conformance][terrain]") {
    // This test's own history is worth keeping, because it is the shape of
    // the bug it eventually found. It was first written to say WHERE the
    // heightmap test's residual lived: the overworld's interpolation cell is
    // 4 wide and 8 tall, `(y + 64) % 8 == 0` is a cell boundary — the one
    // place `minecraft:interpolated` returns its argument rather than a
    // blend — and this test's original sample found that offset clean and
    // every other offset not. Widening the sample later found corner
    // (offset-0) disagreements too, which SPEC §11 first read as "the
    // interpolation isn't the whole story" and only later, correctly, as
    // this build's OWN cell-corner computation being wrong before any
    // interpolation ran at all.
    //
    // Neither reading was the mechanism. It was one missing epsilon in
    // `PerlinNoise::sample`'s fold, inside `old_blended_noise`'s Modern
    // reading (`base_3d_noise`) — found via a clean-room spec
    // (`spec/blended-noise-spec.md` Q2.2/Q2.3) and confirmed directly
    // against the server (`tools/analysis/final-density-probe.sh`). Fixed,
    // the asymmetry this test was built to characterise is gone: every
    // block agrees, not just the corners, not just most of the offsets.
    //
    // What remains worth keeping is the INSTRUMENT — this checks the
    // server's placed block against this build's density sign at every
    // block in range, not just a column's topmost solid the way the
    // heightmap test above does. That is strictly more than the heightmap
    // test can see, and it is what should trip first if this regresses.
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

    // A band covering ordinary overworld terrain elevation — including y=24
    // and y=48, both cell corners the fold's missing epsilon used to get
    // wrong before it was found — while keeping the test to about fifty
    // thousand evaluations rather than the whole column height.
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

    // The test has to be exercising something, or the claims below are
    // vacuous — the sample needs to actually reach the cell boundary.
    REQUIRE(boundaryBlocks > 10000);

    // The claim, now symmetric: EXACT agreement, at corners and between
    // them alike. Before the fold's epsilon this was 0 on-boundary against
    // a nonzero between-boundaries count — the asymmetry this test used to
    // exist to characterise. If either ever becomes non-zero again, SPEC
    // §11's fold-epsilon fix has regressed or a new mechanism has appeared.
    CHECK(onBoundary == 0);
    CHECK(betweenBoundaries == 0);
}
