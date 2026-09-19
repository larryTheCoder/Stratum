// Stratum — scores tools/analysis/legacy-blended-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Inverts every cell-corner column of each probed dimension into a reading of
// `old_blended_noise` at (x, 0, z) — see density-probe.sh's header for the
// trick — and asks which candidate seeding reproduces it, column by column.
//
// The candidates, all three built from the world seed alone:
//
//   legacy+modern  BlendedNoise::legacyFromWorldSeed — new java.util.Random
//                  (worldSeed) straight in, modern reading
//   legacy         BlendedNoise::legacy — the same draws, pre-1.18 reading
//   modern         BlendedNoise::modern — the Xoroshiro factory under
//                  "minecraft:terrain", which is what a flag-off dimension uses
//
// A column COUNTS as agreement when the candidate's value lands within half a
// quantum of the inverted reading, a quantum being one block of terrain —
// 2 / height / (K * scale). That is the floor of what the readback can
// resolve, so the criterion is "agrees to the limit of the instrument", not a
// tolerance anyone picked.
//
// Columns whose terrain reaches the top or the bottom of the world are
// CLIPPED: the inversion has nothing to say about them, they are reported
// separately, and they are excluded from the denominator rather than scored
// as disagreement, which would flatter whichever candidate is wrong.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated
//       tools/analysis/legacy-blended-analyze.cpp -L build/dev/lib -lstratum_core -lz
//       -o build/legacy-blended-analyze
//   build/legacy-blended-analyze .fixtures/1.21.11/probes/legblend_s42 42
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/blended.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace stratum;

namespace {

// Must match tools/analysis/density-probe.sh and legacy-blended-probe.sh.
constexpr double kK = 0.35;
constexpr double kMinY = -64.0;
constexpr double kHeight = 384.0;

const noise::BlendedNoise::Parameters kOverworldShape{.xzScale = 0.25,
                                                      .yScale = 0.375,
                                                      .xzFactor = 80.0,
                                                      .yFactor = 60.0,
                                                      .smearScaleMultiplier = 8.0};

struct Dimension {
    const char* name;
    bool legacy;
    double scale;
};

// The four dimensions legacy-blended-probe.sh writes, in its order.
constexpr std::array<Dimension, 4> kDimensions{{
    {"leg_fine", true, 2.0},
    {"leg_coarse", true, 0.5},
    {"mod_fine", false, 2.0},
    {"mod_coarse", false, 0.5},
}};

struct Score {
    std::size_t agreed = 0;
    double worst = 0.0;
};

/// Every cell-corner column of a probe region, as a reading of the function.
/// `clipped` counts the columns the inversion cannot speak for.
[[nodiscard]] std::vector<std::pair<std::pair<std::int32_t, std::int32_t>, double>>
readColumns(const std::filesystem::path& region, double scale, std::size_t& clipped) {
    std::vector<std::pair<std::pair<std::int32_t, std::int32_t>, double>> out;
    const region::RegionFile file = region::RegionFile::open(region);
    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const chunk::Chunk decoded =
                chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    // Cell corners only: everywhere else carries an
                    // interpolated value rather than the function's own.
                    if ((x % 4) != 0 || (z % 4) != 0) {
                        continue;
                    }
                    int surface = static_cast<int>(kMinY) - 1;
                    for (int y = static_cast<int>(kMinY + kHeight) - 1; y >= kMinY; --y) {
                        const chunk::BlockState* block = decoded.blockAt(localX, y, localZ);
                        if (block != nullptr && block->name != "minecraft:air") {
                            surface = y;
                            break;
                        }
                    }
                    if (surface <= static_cast<int>(kMinY) ||
                        surface >= static_cast<int>(kMinY + kHeight) - 1) {
                        ++clipped;
                        continue;
                    }
                    const double gradient =
                        1.0 - (2.0 * ((surface + 0.5) - kMinY) / kHeight);
                    out.emplace_back(std::pair{x, z}, -gradient / (kK * scale));
                }
            }
        }
    }
    return out;
}

[[nodiscard]] Score score(
    const std::vector<std::pair<std::pair<std::int32_t, std::int32_t>, double>>& columns,
    const noise::BlendedNoise& candidate, double quantum) {
    Score result;
    for (const auto& [position, theirs] : columns) {
        const double ours =
            candidate.sample(static_cast<double>(position.first), 0.0,
                             static_cast<double>(position.second));
        const double error = std::abs(ours - theirs);
        result.worst = std::max(result.worst, error);
        if (error <= (0.5 * quantum) + 1e-12) {
            ++result.agreed;
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <probe dir> <world seed>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    const std::int64_t seed = std::strtoll(argv[2], nullptr, 10);

    std::printf("%-12s  %-8s  %10s  %10s  %10s  %8s  %s\n", "dimension", "declares",
                "legacy+mod", "legacy", "modern", "clipped", "worst(legacy+mod)");

    std::array<std::size_t, 3> totals{};
    std::size_t columnsTotal = 0;
    for (const Dimension& dimension : kDimensions) {
        const std::filesystem::path region = root / dimension.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            std::fprintf(stderr, "missing %s\n", region.string().c_str());
            return 1;
        }
        std::size_t clipped = 0;
        const auto columns = readColumns(region, dimension.scale, clipped);
        const double quantum = 2.0 / kHeight / (kK * dimension.scale);

        rng::JavaRandom preModernRandom{seed};
        const std::array<noise::BlendedNoise, 3> candidates{
            noise::BlendedNoise::legacyFromWorldSeed(seed, kOverworldShape),
            noise::BlendedNoise::legacy(preModernRandom, kOverworldShape),
            noise::BlendedNoise::modern(seed, kOverworldShape),
        };
        std::array<Score, 3> scores{};
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            scores[i] = score(columns, candidates[i], quantum);
            totals[i] += scores[i].agreed;
        }
        columnsTotal += columns.size();
        std::printf("%-12s  %-8s  %5zu/%-4zu  %5zu/%-4zu  %5zu/%-4zu  %8zu  %.5f (q/2 %.5f)\n",
                    dimension.name, dimension.legacy ? "legacy" : "modern", scores[0].agreed,
                    columns.size(), scores[1].agreed, columns.size(), scores[2].agreed,
                    columns.size(), clipped, scores[0].worst, 0.5 * quantum);
    }
    std::printf("total %zu columns: legacy+mod %zu, legacy %zu, modern %zu\n", columnsTotal,
                totals[0], totals[1], totals[2]);
    return 0;
}
