// Stratum — old_blended_noise, measured from the vanilla server itself.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The seeding was found against deepslate, and deepslate is an emulator. This
// is the same claim checked against Mojang's own binary, which SPEC §7 makes
// the authority — and it is checked without asking the server for a number it
// has no way to report.
//
// tools/analysis/blended-datapack-probe.sh builds a datapack whose dimension
// has, as its entire final_density,
//
//     K * flat_cache(old_blended_noise) + y_clamped_gradient(+1 .. -1)
//
// A block is placed where that is positive. `flat_cache` pins the noise to
// y = 0 for the whole column, so the only thing varying with height is the
// gradient and the surface sits exactly where K*N + g(y) = 0. Invert g and
// every column's terrain height *is* a reading of N(x, 0, z), written by the
// server. Aquifers, ore veins, carvers and features are all off — the biome is
// one the pack defines with empty `carvers` and `features`, because
// minecraft:plains carves caves through the terrain and puts lakes on top of
// it, and the first run of this probe duly found its outliers topped with
// water.
//
// The fixture is Mojang-derived and never committed. Without it this skips.
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/blended.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Must match tools/analysis/blended-datapack-probe.sh.
constexpr double kScale = 1.5;
constexpr double kMinY = -64.0;
constexpr double kHeight = 384.0;
constexpr std::int64_t kSeed = 42;

[[nodiscard]] std::filesystem::path probeRegion() {
    // Same shape as the other fixtures: version, then what it is.
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11" / "probes" / "blended-noise" /
           ("seed-" + std::to_string(kSeed)) / "r.0.0.mca";
}

} // namespace

TEST_CASE("the blended noise the server itself writes is the one this build computes",
          "[conformance][blended]") {
    const std::filesystem::path region = probeRegion();
    if (!std::filesystem::is_regular_file(region)) {
        SKIP("no probe fixture at " << region
                                    << "; generate it with "
                                       "tools/analysis/blended-datapack-probe.sh --accept-eula");
    }

    const stratum::noise::BlendedNoise predicted = stratum::noise::BlendedNoise::modern(
        kSeed, stratum::noise::BlendedNoise::Parameters{.xzScale = 0.25,
                                                        .yScale = 0.125,
                                                        .xzFactor = 80.0,
                                                        .yFactor = 160.0,
                                                        .smearScaleMultiplier = 8.0});

    const stratum::region::RegionFile file = stratum::region::RegionFile::open(region);

    // One block of terrain is worth this much of N, and no reading taken this
    // way can do better than half of it — the surface is a block, and its
    // centre is the best estimate of where the density crossed zero.
    const double quantum = 2.0 / kHeight / kScale;

    std::size_t columns = 0;
    std::size_t withinHalfABlock = 0;
    double worst = 0.0;
    double sumOurs = 0.0;
    double sumTheirs = 0.0;

    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const stratum::chunk::Chunk chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);

            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;

                    // Cell corners only. final_density is evaluated on the
                    // cell lattice and interpolated across it, and flat_cache
                    // pins its argument to the 4x4 column corner as well, so
                    // anywhere else carries an interpolated value rather than
                    // the noise. Comparing those against the noise at that
                    // column compares two different quantities — which the
                    // first run of this did, at a cost of 0.04 of correlation.
                    if ((x % 4) != 0 || (z % 4) != 0) {
                        continue;
                    }

                    int surface = static_cast<int>(kMinY) - 1;
                    for (int y = static_cast<int>(kMinY + kHeight) - 1; y >= kMinY; --y) {
                        const stratum::chunk::BlockState* block = chunk.blockAt(localX, y, localZ);
                        if (block != nullptr && block->name != "minecraft:air") {
                            surface = y;
                            break;
                        }
                    }
                    REQUIRE(surface >= kMinY);

                    const double gradient = 1.0 - (2.0 * ((surface + 0.5) - kMinY) / kHeight);
                    const double theirs = -gradient / kScale;
                    const double ours = predicted.sample(x, 0.0, z);

                    ++columns;
                    sumOurs += ours;
                    sumTheirs += theirs;
                    const double error = std::abs(ours - theirs);
                    worst = std::max(worst, error);
                    if (error <= 0.5 * quantum + 1e-12) {
                        ++withinHalfABlock;
                    }
                }
            }
        }
    }

    // A full region's worth of cell corners: 512 blocks square, every fourth.
    REQUIRE(columns == 16384U);

    // Every column, to within half a block — which is the floor of what this
    // measurement can resolve, so this is agreement to the limit of the
    // instrument rather than agreement within a tolerance someone chose.
    CHECK(withinHalfABlock == columns);
    CHECK(worst <= 0.5 * quantum + 1e-12);

    // And the fields have the same mean, which the per-column check would not
    // catch if both sides were biased the same way by the inversion.
    CHECK(std::abs((sumOurs - sumTheirs) / static_cast<double>(columns)) < 1.0e-5);
}
