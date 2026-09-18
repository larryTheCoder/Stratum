// Stratum — how a legacy dimension seeds `old_blended_noise`, off the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `minecraft:old_blended_noise` is the one noise a `legacy_random_source`
// dimension can seed without first answering the question SPEC §11 leaves
// open — how a NAMED noise's identifier becomes a Java LCG seed. It carries
// no identifier, so there is nothing to hash.
//
// The rule, measured: `new java.util.Random(worldSeed)` handed straight in —
// no positional fork, no name salt — with the three octave stacks drawn in
// the order `BlendedNoise::legacy` already draws them, and the value read the
// MODERN way rather than the pre-1.18 way. The two readings differ by a
// factor of exactly 128 at y = 0, so nothing here is a rounding question.
// `BlendedNoise::legacyFromWorldSeed` is that rule.
//
// THE READBACK is tools/analysis/density-probe.sh's: the dimension's whole
// `final_density` is `K * flat_cache(scale * old_blended_noise) +
// y_clamped_gradient(+1..-1)`, so a cell-corner column's terrain height
// inverts to a reading of the noise at (x, 0, z). See that script's header.
//
// WHAT MAKES IT MORE THAN A FIT. Each world carries the same two functions
// twice — once in a dimension declaring the flag and once in one that does
// not — and the two candidate seedings trade places between them. A rule that
// won because of the readback rather than because of the seeding would score
// the same on both sides. Both directions are checked here, and the one that
// loses is required to lose *badly*, not merely to lose.
//
// THE NULL IS PER-DIMENSION, not a single number. A column counts as
// agreement within half a quantum, a quantum being one block of terrain —
// 2 / height / (K * scale) — so a coarser output scale accepts a wider band
// and a wrong candidate scores higher there for no other reason. That is why
// each world carries both a fine and a coarse copy, and why this file asserts
// the loser's score against a per-dimension ceiling rather than against one
// global "empirical null". Quoting a single percentage across dimensions of
// different resolutions is how a 16% outlier gets mistaken for a signal.
//
// The fixtures come from tools/analysis/legacy-blended-probe.sh and are
// Mojang-derived, so they are never committed (SPEC §12). Without them this
// skips.
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/blended.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

// Must match tools/analysis/density-probe.sh and legacy-blended-probe.sh.
constexpr double kK = 0.35;
constexpr int kMinY = -64;
constexpr int kHeight = 384;

// Vanilla's own overworld numbers for this node, which is what the probe
// configures it with.
const stratum::noise::BlendedNoise::Parameters kShape{.xzScale = 0.25,
                                                      .yScale = 0.375,
                                                      .xzFactor = 80.0,
                                                      .yFactor = 60.0,
                                                      .smearScaleMultiplier = 8.0};

struct Dimension {
    const char* name;
    bool legacy;
    double scale;
};

constexpr std::array<Dimension, 4> kDimensions{{
    {"leg_fine", true, 2.0},
    {"leg_coarse", true, 0.5},
    {"mod_fine", false, 2.0},
    {"mod_coarse", false, 0.5},
}};

// The seeds legacy-blended-probe.sh was run at. Three, because one world seed
// agreeing with an RNG-driven derivation is not evidence that the derivation
// is right — it is evidence that it is right once.
constexpr std::array<std::int64_t, 3> kSeeds{42, 0, -4172144997902289642LL};

struct Column {
    std::int32_t x = 0;
    std::int32_t z = 0;
    double value = 0.0;
};

[[nodiscard]] std::filesystem::path probeRoot(std::int64_t seed) {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" /
           ("legblend_s" + std::to_string(seed));
}

/// Every cell-corner column of one probe region, inverted into a reading of
/// the function. Columns with no block at all belong to chunks the server
/// left empty around the forceloaded square; they carry no reading and are
/// dropped rather than counted as disagreement, which would flatter whichever
/// candidate is wrong.
[[nodiscard]] std::vector<Column> readColumns(const std::filesystem::path& region, double scale) {
    std::vector<Column> columns;
    const stratum::region::RegionFile file = stratum::region::RegionFile::open(region);
    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const stratum::chunk::Chunk decoded = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    // Cell corners only: `final_density` is evaluated on the
                    // cell lattice and interpolated across it, and
                    // `flat_cache` pins its argument to the 4x4 column corner
                    // as well, so anywhere else carries an interpolated value
                    // rather than the function's own.
                    if ((x % 4) != 0 || (z % 4) != 0) {
                        continue;
                    }
                    int surface = kMinY - 1;
                    for (int y = kMinY + kHeight - 1; y >= kMinY; --y) {
                        const stratum::chunk::BlockState* block =
                            decoded.blockAt(localX, y, localZ);
                        if (block != nullptr && block->name != "minecraft:air") {
                            surface = y;
                            break;
                        }
                    }
                    if (surface <= kMinY || surface >= kMinY + kHeight - 1) {
                        continue;
                    }
                    const double gradient =
                        1.0 - (2.0 * ((surface + 0.5) - kMinY) / static_cast<double>(kHeight));
                    columns.push_back({x, z, -gradient / (kK * scale)});
                }
            }
        }
    }
    return columns;
}

[[nodiscard]] std::size_t agreeing(const std::vector<Column>& columns,
                                   const stratum::noise::BlendedNoise& candidate, double quantum) {
    std::size_t agreed = 0;
    for (const Column& column : columns) {
        const double ours =
            candidate.sample(static_cast<double>(column.x), 0.0, static_cast<double>(column.z));
        if (std::abs(ours - column.value) <= (0.5 * quantum) + 1e-12) {
            ++agreed;
        }
    }
    return agreed;
}

} // namespace

TEST_CASE("a legacy dimension's old_blended_noise is the world seed straight into the LCG",
          "[conformance][blended][legacy]") {
    std::size_t worlds = 0;
    std::size_t matchedTotal = 0;
    std::size_t columnsTotal = 0;

    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path root = probeRoot(seed);
        if (!std::filesystem::is_directory(root)) {
            continue;
        }
        ++worlds;
        for (const Dimension& dimension : kDimensions) {
            const std::filesystem::path region = root / dimension.name / "r.0.0.mca";
            REQUIRE(std::filesystem::is_regular_file(region));

            const std::vector<Column> columns = readColumns(region, dimension.scale);
            // The probe forceloads 8x8 chunks, which is 32x32 cell corners;
            // the server generates a wider square around them, and every
            // generated column carries a reading. Anything much smaller means
            // the world did not finish.
            REQUIRE(columns.size() >= 1024U);

            const double quantum = 2.0 / static_cast<double>(kHeight) / (kK * dimension.scale);

            stratum::rng::JavaRandom preModern{seed};
            const stratum::noise::BlendedNoise legacyModern =
                stratum::noise::BlendedNoise::legacyFromWorldSeed(seed, kShape);
            const stratum::noise::BlendedNoise legacyPreModern =
                stratum::noise::BlendedNoise::legacy(preModern, kShape);
            const stratum::noise::BlendedNoise modern =
                stratum::noise::BlendedNoise::modern(seed, kShape);

            const std::size_t byLegacyModern = agreeing(columns, legacyModern, quantum);
            const std::size_t byLegacyPreModern = agreeing(columns, legacyPreModern, quantum);
            const std::size_t byModern = agreeing(columns, modern, quantum);

            const std::size_t expected = dimension.legacy ? byLegacyModern : byModern;
            const std::size_t rival = dimension.legacy ? byModern : byLegacyModern;

            INFO("seed " << seed << " dimension " << dimension.name << ": legacy+modern "
                         << byLegacyModern << ", modern " << byModern << ", legacy(pre-1.18) "
                         << byLegacyPreModern << " of " << columns.size());

            // The rule the dimension declares reproduces EVERY column, to the
            // limit of what a terrain-height readback can resolve.
            CHECK(expected == columns.size());

            // And the other one does not come close. The ceiling is a tenth
            // of the columns: the two rivals measure 1.4-1.6% at the fine
            // scale and 4.9-5.9% at the coarse one, and the gap between those
            // two numbers is the quantum, not the seeding.
            CHECK(rival < columns.size() / 10);

            // The pre-1.18 reading is refuted on both sides of the mirror: it
            // shares every draw with the winner in the legacy dimensions and
            // still lands at the floor, because the readings differ by 128x.
            CHECK(byLegacyPreModern < columns.size() / 100);

            matchedTotal += expected;
            columnsTotal += columns.size();
        }
    }

    if (worlds == 0) {
        SKIP("no legblend_s* probe fixtures under "
             << std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes"
             << "; generate them with tools/analysis/legacy-blended-probe.sh --accept-eula "
                "<seed> for seeds 42, 0 and -4172144997902289642");
    }

    // Stated as a count rather than a rate, so a world that half-generated
    // cannot pass by shrinking the denominator.
    CHECK(matchedTotal == columnsTotal);
}
