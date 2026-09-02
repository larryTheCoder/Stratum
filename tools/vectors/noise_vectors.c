/* Stratum — known-answer vectors for stratum::noise, from cubiomes.
 * Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
 *
 * cubiomes (MIT, Cubitect) is the reference SPEC §2 names for noise and biome
 * math. It is an independent reimplementation of Minecraft's noise, so
 * agreement with it is strong evidence that our implementation is right —
 * not proof that either matches Mojang. That is settled by diffing generated
 * terrain against the goldens (M3).
 *
 * cubiomes is fetched at a pinned commit by
 * tools/vectors/generate-noise-vectors.sh and is never vendored into this
 * repository.
 *
 * Doubles are emitted as raw bit patterns: noise parity is bit-exact or it
 * is nothing.
 */

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

static const uint64_t kSeeds[] = {
    0ULL, 1ULL, 42ULL, (uint64_t)-1LL, 123456789ULL, (uint64_t)-4172144997902289642LL,
};
static const int kSeedCount = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

/* Coordinates that exercise the lattice: exact integers, negatives, and
 * points well inside a cell. */
static const double kCoords[][3] = {
    {0.0, 0.0, 0.0},        {1.0, 0.0, 0.0},        {0.5, 0.5, 0.5},
    {-1.0, -1.0, -1.0},     {-0.5, 2.25, 13.75},    {123.456, 0.0, -78.9},
    {1000.5, 64.0, -1000.5},{0.0, 100.0, 0.0},      {-4096.25, -60.5, 4096.75},
};
static const int kCoordCount = (int)(sizeof(kCoords) / sizeof(kCoords[0]));

static void emitXNextInt(void) {
    static const uint32_t bounds[] = {1, 2, 3, 16, 17, 256, 255, 1000, 0x7FFFFFFF};
    const int boundCount = (int)(sizeof(bounds) / sizeof(bounds[0]));

    printf("struct XNextIntVector {\n");
    printf("    std::uint64_t lo;\n");
    printf("    std::uint64_t hi;\n");
    printf("    std::int32_t bound;\n");
    printf("    std::array<std::int32_t, 8> values;\n");
    printf("};\n\n");
    printf("constexpr auto kXNextIntVectors = std::to_array<XNextIntVector>({\n");
    for (int s = 0; s < kSeedCount; s++) {
        for (int b = 0; b < boundCount; b++) {
            Xoroshiro xr;
            xSetSeed(&xr, kSeeds[s]);
            uint64_t lo = xr.lo, hi = xr.hi;
            printf("    {UINT64_C(0x%016llX), UINT64_C(0x%016llX), %d, {{",
                   (unsigned long long)lo, (unsigned long long)hi, (int)bounds[b]);
            for (int i = 0; i < 8; i++) {
                printf("%s%d", i ? ", " : "", xNextInt(&xr, bounds[b]));
            }
            printf("}}},\n");
        }
    }
    printf("});\n\n");
}

static void emitPerlinInit(void) {
    printf("struct PerlinInitVector {\n");
    printf("    std::uint64_t seed;\n");
    printf("    std::uint64_t originX;\n");
    printf("    std::uint64_t originY;\n");
    printf("    std::uint64_t originZ;\n");
    printf("    std::array<std::uint8_t, 256> permutation;\n");
    printf("};\n\n");
    printf("constexpr auto kPerlinInitVectors = std::to_array<PerlinInitVector>({\n");
    for (int s = 0; s < kSeedCount; s++) {
        Xoroshiro xr;
        PerlinNoise noise;
        xSetSeed(&xr, kSeeds[s]);
        xPerlinInit(&noise, &xr);

        printf("    {UINT64_C(0x%016llX), ", (unsigned long long)kSeeds[s]);
        printDouble(noise.a); printf(", ");
        printDouble(noise.b); printf(", ");
        printDouble(noise.c); printf(", {{");
        for (int i = 0; i < 256; i++) {
            printf("%s%d", i ? "," : "", (int)noise.d[i]);
        }
        printf("}}},\n");
    }
    printf("});\n\n");
}

static void emitPerlinSamples(void) {
    printf("struct PerlinSampleVector {\n");
    printf("    std::uint64_t seed;\n");
    printf("    std::uint64_t x;\n");
    printf("    std::uint64_t y;\n");
    printf("    std::uint64_t z;\n");
    printf("    std::uint64_t value;\n");
    printf("};\n\n");
    printf("constexpr auto kPerlinSampleVectors = std::to_array<PerlinSampleVector>({\n");
    for (int s = 0; s < kSeedCount; s++) {
        Xoroshiro xr;
        PerlinNoise noise;
        xSetSeed(&xr, kSeeds[s]);
        xPerlinInit(&noise, &xr);
        for (int c = 0; c < kCoordCount; c++) {
            const double value =
                samplePerlin(&noise, kCoords[c][0], kCoords[c][1], kCoords[c][2], 0.0, 0.0);
            printf("    {UINT64_C(0x%016llX), ", (unsigned long long)kSeeds[s]);
            printDouble(kCoords[c][0]); printf(", ");
            printDouble(kCoords[c][1]); printf(", ");
            printDouble(kCoords[c][2]); printf(", ");
            printDouble(value); printf("},\n");
        }
    }
    printf("});\n\n");
}

static void emitSimplexSamples(void) {
    printf("struct SimplexSampleVector {\n");
    printf("    std::uint64_t seed;\n");
    printf("    std::uint64_t x;\n");
    printf("    std::uint64_t y;\n");
    printf("    std::uint64_t value;\n");
    printf("};\n\n");
    printf("constexpr auto kSimplexSampleVectors = std::to_array<SimplexSampleVector>({\n");
    for (int s = 0; s < kSeedCount; s++) {
        Xoroshiro xr;
        PerlinNoise noise;
        xSetSeed(&xr, kSeeds[s]);
        xPerlinInit(&noise, &xr);
        for (int c = 0; c < kCoordCount; c++) {
            const double value = sampleSimplex2D(&noise, kCoords[c][0], kCoords[c][2]);
            printf("    {UINT64_C(0x%016llX), ", (unsigned long long)kSeeds[s]);
            printDouble(kCoords[c][0]); printf(", ");
            printDouble(kCoords[c][2]); printf(", ");
            printDouble(value); printf("},\n");
        }
    }
    printf("});\n\n");
}

/* Octave and double-perlin configurations shaped like the ones vanilla's
 * noise router actually uses. */
static const double kAmplitudesA[] = {1.0, 1.0, 1.0, 1.0};
static const double kAmplitudesB[] = {1.0, 1.0, 2.0, 2.0, 1.0};
static const double kAmplitudesC[] = {1.0, 0.0, 1.0};

struct OctaveConfig {
    const double *amplitudes;
    int length;
    int firstOctave;
};

static void emitOctaveAndDouble(void) {
    const struct OctaveConfig configs[] = {
        {kAmplitudesA, 4, -7},
        {kAmplitudesB, 5, -9},
        {kAmplitudesC, 3, -3},
        {kAmplitudesA, 4, -4},
        {kAmplitudesC, 3, -12},
    };
    /* Every octave index must stay within [-12, 0]: cubiomes precomputes the
     * MD5 octave salts for exactly that range and indexes the table as
     * 12 + omin + i, so a configuration reaching octave_1 reads off the end
     * of it and produces garbage. Vanilla hashes the name at run time and has
     * no such limit, and neither do we — which is why that case cannot be
     * covered by this oracle rather than why it is skipped. */
    const int configCount = (int)(sizeof(configs) / sizeof(configs[0]));

    printf("struct OctaveSampleVector {\n");
    printf("    std::uint64_t seed;\n");
    printf("    int firstOctave;\n");
    printf("    std::array<std::uint64_t, 5> amplitudes;\n");
    printf("    int amplitudeCount;\n");
    printf("    std::uint64_t x;\n");
    printf("    std::uint64_t y;\n");
    printf("    std::uint64_t z;\n");
    printf("    std::uint64_t octaveValue;\n");
    printf("    std::uint64_t doublePerlinValue;\n");
    printf("};\n\n");
    printf("constexpr auto kOctaveSampleVectors = std::to_array<OctaveSampleVector>({\n");

    for (int s = 0; s < kSeedCount; s++) {
        for (int k = 0; k < configCount; k++) {
            PerlinNoise octaveStorage[32];
            PerlinNoise doubleStorage[64];
            OctaveNoise octave;
            DoublePerlinNoise doublePerlin;
            Xoroshiro xr;

            /* nmax = -1 means "no limit". Passing 0 initialises no octaves
             * at all and quietly yields a generator that returns zero. */
            xSetSeed(&xr, kSeeds[s]);
            xOctaveInit(&octave, &xr, octaveStorage, configs[k].amplitudes,
                        configs[k].firstOctave, configs[k].length, -1);

            xSetSeed(&xr, kSeeds[s]);
            xDoublePerlinInit(&doublePerlin, &xr, doubleStorage, configs[k].amplitudes,
                              configs[k].firstOctave, configs[k].length, -1);

            for (int c = 0; c < kCoordCount; c++) {
                const double octaveValue =
                    sampleOctave(&octave, kCoords[c][0], kCoords[c][1], kCoords[c][2]);
                const double doubleValue = sampleDoublePerlin(
                    &doublePerlin, kCoords[c][0], kCoords[c][1], kCoords[c][2]);

                printf("    {UINT64_C(0x%016llX), %d, {{", (unsigned long long)kSeeds[s],
                       configs[k].firstOctave);
                for (int a = 0; a < 5; a++) {
                    if (a) printf(", ");
                    printDouble(a < configs[k].length ? configs[k].amplitudes[a] : 0.0);
                }
                printf("}}, %d, ", configs[k].length);
                printDouble(kCoords[c][0]); printf(", ");
                printDouble(kCoords[c][1]); printf(", ");
                printDouble(kCoords[c][2]); printf(", ");
                printDouble(octaveValue); printf(", ");
                printDouble(doubleValue); printf("},\n");
            }
        }
    }
    printf("});\n\n");
}

int main(void) {
    printf("// GENERATED FILE — DO NOT EDIT BY HAND.\n");
    printf("//\n");
    printf("// Known-answer vectors for stratum::noise, produced by cubiomes (MIT),\n");
    printf("// the noise reference SPEC §2 names. Doubles are raw bit patterns.\n");
    printf("// Regenerate with: tools/vectors/generate-noise-vectors.sh\n");
    printf("//\n");
    printf("// cubiomes is an independent reimplementation, so agreement with it is\n");
    printf("// strong evidence, not proof that either matches Mojang. That is settled\n");
    printf("// by diffing generated terrain against the goldens (M3).\n");
    printf("\n");
    printf("#include <array>\n");
    printf("#include <cstdint>\n");
    printf("\n");
    printf("// clang-format off\n\n");

    emitXNextInt();
    emitPerlinInit();
    emitPerlinSamples();
    emitSimplexSamples();
    emitOctaveAndDouble();

    printf("// clang-format on\n");
    return 0;
}
