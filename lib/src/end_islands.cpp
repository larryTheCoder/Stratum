// Stratum — the End's island field.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// See end_islands.hpp for what is measured here and what is not.

#include <stratum/javamath.hpp>
#include <stratum/noise/end_islands.hpp>
#include <stratum/rng/java_random.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stratum::noise {

namespace {

/// The neighbourhood of island cells a single column can be reached by.
constexpr std::int32_t kCellRadius = 12;
/// Below this squared cell distance no outer island exists: the central
/// island owns the middle of the dimension outright.
constexpr std::int64_t kInnerVoidRadiusSquared = 4096;
/// How selective the simplex gate is. Roughly one cell in a hundred.
constexpr double kIslandThreshold = -0.9;
/// LCG steps discarded between seeding with the world seed and building the
/// simplex. See fromWorldSeed.
constexpr int kSeedSkip = 17292;

/// Java's `Mth.clamp` on floats, spelled out because the bounds are
/// asymmetric and are part of the measured shape rather than of taste.
[[nodiscard]] float clampHeight(float value) noexcept {
    return std::min(80.0F, std::max(-100.0F, value));
}

} // namespace

EndIslands EndIslands::fromWorldSeed(const std::int64_t worldSeed) {
    // The world seed into the LCG, THEN 17292 steps thrown away before the
    // simplex is built. Measured, not reasoned: without the skip the island
    // positions are uncorrelated with vanilla's, and with it the field agrees
    // on every column the instrument can speak for (see the header, and
    // tools/analysis/end-islands-analyze.cpp).
    //
    // The 17292 is a literal with no derivation behind it — it is what the
    // measurement says, and nothing about the rest of the field predicts it.
    // So the search that found it is kept runnable rather than remembered:
    // `end-islands-analyze <probe> <seed> 0 65536` rescores every skip in
    // that range against the server's own field. Over 65537 candidates,
    // EXACTLY ONE reaches 1024 of 1024 columns, at both seeds probed:
    //
    //   seed 0   best 17292 at 1024/1024, next best 920, 94 past the prefix
    //   seed 42  best 17292 at 1024/1024, next best 908, 141 past the prefix
    //
    // Read that gap for what it is. A wrong skip is not near-random here —
    // scores of 900 are common — because a 128-block window is mostly flat
    // field that many wrong island layouts also get right. Full agreement is
    // the only discriminating outcome, and one candidate has it, twice.
    rng::JavaRandom random{worldSeed};
    for (int i = 0; i < kSeedSkip; ++i) {
        static_cast<void>(random.nextInt());
    }
    return EndIslands{PerlinNoise::fromRandom(random)};
}

float EndIslands::centralIslandHeight(const std::int32_t gridX, const std::int32_t gridZ) noexcept {
    const auto squared =
        (static_cast<std::int64_t>(gridX) * gridX) + (static_cast<std::int64_t>(gridZ) * gridZ);
    const auto distance = static_cast<float>(std::sqrt(static_cast<double>(squared)));
    return clampHeight(100.0F - (distance * 8.0F));
}

double EndIslands::sampleSimplexSeeding(const std::int32_t cellX,
                                        const std::int32_t cellZ) const noexcept {
    return source_.sampleSimplex2D(static_cast<double>(cellX), static_cast<double>(cellZ));
}

float EndIslands::heightAt(const std::int32_t gridX, const std::int32_t gridZ) const noexcept {
    // floorDiv/floorMod, not `/` and `%`: the grid runs through the origin
    // and the field is NOT symmetric under truncation — Java's own `/` here
    // is applied to a coordinate that has already been floor-divided by 8 on
    // the way in, and the two conventions disagree for every negative
    // coordinate (CLAUDE.md's determinism rules).
    const std::int32_t cellX = javamath::floorDiv(gridX, 2);
    const std::int32_t cellZ = javamath::floorDiv(gridZ, 2);
    const std::int32_t withinX = javamath::floorMod(gridX, 2);
    const std::int32_t withinZ = javamath::floorMod(gridZ, 2);

    float height = centralIslandHeight(gridX, gridZ);

    for (std::int32_t offsetX = -kCellRadius; offsetX <= kCellRadius; ++offsetX) {
        for (std::int32_t offsetZ = -kCellRadius; offsetZ <= kCellRadius; ++offsetZ) {
            const auto islandX = static_cast<std::int64_t>(cellX) + offsetX;
            const auto islandZ = static_cast<std::int64_t>(cellZ) + offsetZ;
            if ((islandX * islandX) + (islandZ * islandZ) <= kInnerVoidRadiusSquared) {
                continue;
            }
            if (source_.sampleSimplex2D(static_cast<double>(islandX),
                                        static_cast<double>(islandZ)) >= kIslandThreshold) {
                continue;
            }
            // The island's own steepness, a deterministic hash of its cell.
            const auto steepness = static_cast<float>(
                (((std::abs(islandX) * 3439) + (std::abs(islandZ) * 147)) % 13) + 9);
            const auto localX = static_cast<float>(withinX - (offsetX * 2));
            const auto localZ = static_cast<float>(withinZ - (offsetZ * 2));
            const auto distance = static_cast<float>(
                std::sqrt(static_cast<double>((localX * localX) + (localZ * localZ))));
            height = std::max(height, clampHeight(100.0F - (distance * steepness)));
        }
    }
    return height;
}

double EndIslands::sample(const std::int32_t blockX, const std::int32_t blockZ) const noexcept {
    const float height = heightAt(javamath::floorDiv(blockX, 8), javamath::floorDiv(blockZ, 8));
    return (static_cast<double>(height) - 8.0) / 128.0;
}

} // namespace stratum::noise
