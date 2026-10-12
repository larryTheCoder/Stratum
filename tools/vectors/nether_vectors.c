/* Stratum — known-answer vectors for the legacy Nether climate, from cubiomes.
 * Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
 *
 * cubiomes (MIT, Cubitect) is the reference SPEC §2 names. Its
 * `setNetherSeed` builds the Nether's two climate noises, for every version
 * from 1.16 on, in a way none of this project's legacy-seed scans could
 * reach (SPEC §11, "The legacy Nether's climate, read from cubiomes"):
 *
 *   * temperature from `java.util.Random(worldSeed)` and humidity from
 *     `java.util.Random(worldSeed + 1)` — no name enters, only the seed;
 *   * each a `doublePerlinInit(..., -7, 2)`: two octaves at first octave -7,
 *     amplitudes [1, 1], NOT the pack's `minecraft:temperature` or
 *     `minecraft:vegetation` parameters;
 *   * each stack built by `octaveInit`, which discards 262 LCG steps per
 *     octave above the top one, then draws the highest-frequency octave
 *     first — the opposite order to the one those scans assumed;
 *   * sampled by `getNetherBiome` at (x, 0, z) in quart coordinates, with
 *     no shift at all.
 *
 * WHAT THIS CANNOT COVER, stated here because it is easy to read the
 * agreement below as more than it is:
 *
 *   * These vectors pin cubiomes' construction, not vanilla's. Whether the
 *     two agree is a separate measurement against the golden Nether regions,
 *     made by tools/analysis/legacy-goldens-biome-analyze.cpp --cubiomes.
 *   * Only the two climate values are emitted, not cubiomes' biome choice:
 *     `getNetherBiome` picks with float distances, while vanilla's
 *     multi-noise search is quantised (biome/parameter_list.hpp). The two can
 *     disagree at a boundary, and these vectors take no side on that.
 *   * cubiomes' `maintainPrecision` is a no-op, so the coordinates below are
 *     kept small enough that vanilla's wrap would be the identity too.
 *
 * cubiomes is fetched at a pinned commit by
 * tools/vectors/generate-nether-vectors.sh and is never vendored.
 */

#include "biomenoise.h"
#include "noise.h"
#include "rng.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void printDouble(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    printf("UINT64_C(0x%016llX)", (unsigned long long)bits);
}

/* Long.MIN_VALUE has no literal of its own: `-9223372036854775808` negates a
 * constant that does not fit in a signed 64-bit type, which the compiler
 * warns about and -Werror refuses. */
static void printSeed(uint64_t seed) {
    if (seed == 0x8000000000000000ULL) {
        printf("INT64_MIN");
    } else {
        printf("INT64_C(%lld)", (long long)seed);
    }
}

/* The golden seed set (tools/fetch-vanilla), so that a vector can be read
 * against the same worlds the goldens hold. Long.MAX_VALUE is the one that
 * matters most here: humidity's `worldSeed + 1` wraps it to Long.MIN_VALUE,
 * and an implementation that did that addition in a signed type would be
 * undefined exactly there. */
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

/* Quart coordinates: what `getNetherBiome` takes and what the Nether's
 * router reaches at 0.25 times a block position. Both signs on both axes,
 * the origin, and a few far enough out that every octave's lattice has
 * wrapped its permutation many times. */
static const int kCoords[][2] = {
    {0, 0},       {1, 0},        {0, 1},         {-1, -1},     {3, -7},
    {31, 31},     {127, 0},      {0, -128},      {-300, 451},  {1000, -1000},
    {4096, 17},   {-20000, 9999},
};
static const int kCoordCount = (int)(sizeof(kCoords) / sizeof(kCoords[0]));

int main(void) {
    printf("// GENERATED FILE — DO NOT EDIT BY HAND.\n");
    printf("//\n");
    printf("// Known-answer vectors for the legacy Nether climate, produced by\n");
    printf("// cubiomes (MIT), the noise reference SPEC §2 names: setNetherSeed's\n");
    printf("// temperature and humidity, sampled at quart (x, 0, z). Doubles are raw\n");
    printf("// bit patterns. Regenerate with: tools/vectors/generate-nether-vectors.sh\n");
    printf("//\n");
    printf("// These pin cubiomes' construction. Whether vanilla 1.21.11 agrees is\n");
    printf("// measured separately against the golden Nether (SPEC §11).\n");
    printf("\n");
    printf("#include <array>\n");
    printf("#include <cstdint>\n");
    printf("\n");
    printf("// clang-format off\n\n");

    printf("struct NetherClimateVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    std::int32_t x;\n");
    printf("    std::int32_t z;\n");
    printf("    std::uint64_t temperature;\n");
    printf("    std::uint64_t humidity;\n");
    printf("};\n\n");
    printf("constexpr auto kNetherClimateVectors = std::to_array<NetherClimateVector>({\n");

    for (int s = 0; s < kSeedCount; s++) {
        NetherNoise nn;
        setNetherSeed(&nn, kSeeds[s]);
        for (int k = 0; k < kCoordCount; k++) {
            const double x = kCoords[k][0];
            const double z = kCoords[k][1];
            const double temperature = sampleDoublePerlin(&nn.temperature, x, 0.0, z);
            const double humidity = sampleDoublePerlin(&nn.humidity, x, 0.0, z);
            printf("    {");
            printSeed(kSeeds[s]);
            printf(", %d, %d, ", kCoords[k][0], kCoords[k][1]);
            printDouble(temperature); printf(", ");
            printDouble(humidity); printf("},\n");
        }
    }
    printf("});\n\n");

    printf("// clang-format on\n");
    return 0;
}
