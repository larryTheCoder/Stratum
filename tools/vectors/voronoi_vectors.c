/* Stratum — known-answer vectors for the block-level biome zoom, from
 * cubiomes.
 * Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
 *
 * cubiomes (MIT, Cubitect) is the reference SPEC §2 names. `getVoronoiSHA`
 * turns a world seed into the zoom's 64-bit key, and `voronoiAccess3D` maps
 * a block position to the quart whose biome it reads. biome/voronoi.hpp is
 * adapted from both; these vectors hold the adaptation to them.
 *
 * WHAT THIS CANNOT COVER: whether vanilla 1.21.11's surface rules read the
 * zoomed biome, which is measured separately against the golden Nether
 * (vanilla_legacy_nether_surface_test.cpp; SPEC §11).
 *
 * cubiomes is fetched at a pinned commit by
 * tools/vectors/generate-voronoi-vectors.sh and is never vendored.
 */

#include "layers.h"

#include <stdint.h>
#include <stdio.h>

static void printSeed(uint64_t seed) {
    if (seed == 0x8000000000000000ULL) {
        printf("INT64_MIN");
    } else {
        printf("INT64_C(%lld)", (long long)seed);
    }
}

/* The golden seed set (tools/fetch-vanilla). 0 and Long.MIN_VALUE are one
 * world to java.util.Random, but not to the zoom, which hashes all 64 bits. */
static const uint64_t kSeeds[] = {
    0ULL,
    1ULL,
    (uint64_t)-1LL,
    42ULL,
    (uint64_t)-4172144997902289642LL,
    2891948927356891ULL,
    0x7FFFFFFFFFFFFFFFULL,
    0x8000000000000000ULL,
};
static const int kSeedCount = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

/* Block positions: every offset within a quart on each axis near the origin
 * and across zero, the Nether's floor and roof, and far positions on both
 * signs. */
static const int kPositions[][3] = {
    {0, 0, 0},       {1, 0, 0},        {2, 0, 0},       {3, 0, 0},        {0, 1, 0},
    {0, 2, 0},       {0, 3, 0},        {0, 0, 1},       {0, 0, 2},        {0, 0, 3},
    {-1, -1, -1},    {-2, -3, -4},     {-5, 64, 7},     {15, 127, 15},    {16, 32, -16},
    {100, 31, -100}, {-1415, 40, -602}, {511, 70, 511},  {-512, -64, 300}, {123457, 300, -98765},
    {-29999999, 5, 29999999},
};
static const int kPositionCount = (int)(sizeof(kPositions) / sizeof(kPositions[0]));

int main(void) {
    printf("// GENERATED FILE — DO NOT EDIT BY HAND.\n");
    printf("//\n");
    printf("// Known-answer vectors for the block-level biome zoom, produced by\n");
    printf("// cubiomes (MIT), the reference SPEC §2 names: getVoronoiSHA's key and\n");
    printf("// voronoiAccess3D's quart for each block position.\n");
    printf("// Regenerate with: tools/vectors/generate-voronoi-vectors.sh\n");
    printf("\n");
    printf("#include <array>\n");
    printf("#include <cstdint>\n");
    printf("\n");
    printf("// clang-format off\n\n");
    printf("struct VoronoiSeedVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    std::uint64_t key;\n");
    printf("};\n\n");
    printf("struct VoronoiZoomVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    std::int32_t x, y, z;\n");
    printf("    std::int32_t qx, qy, qz;\n");
    printf("};\n\n");

    printf("constexpr auto kVoronoiSeedVectors = std::to_array<VoronoiSeedVector>({\n");
    for (int s = 0; s < kSeedCount; s++) {
        printf("    {");
        printSeed(kSeeds[s]);
        printf(", UINT64_C(0x%016llX)},\n", (unsigned long long)getVoronoiSHA(kSeeds[s]));
    }
    printf("});\n\n");

    printf("constexpr auto kVoronoiZoomVectors = std::to_array<VoronoiZoomVector>({\n");
    for (int s = 0; s < kSeedCount; s++) {
        const uint64_t sha = getVoronoiSHA(kSeeds[s]);
        for (int p = 0; p < kPositionCount; p++) {
            int qx = 0, qy = 0, qz = 0;
            voronoiAccess3D(sha, kPositions[p][0], kPositions[p][1], kPositions[p][2], &qx, &qy,
                            &qz);
            printf("    {");
            printSeed(kSeeds[s]);
            printf(", %d, %d, %d, %d, %d, %d},\n", kPositions[p][0], kPositions[p][1],
                   kPositions[p][2], qx, qy, qz);
        }
    }
    printf("});\n\n");
    printf("// clang-format on\n");
    return 0;
}
