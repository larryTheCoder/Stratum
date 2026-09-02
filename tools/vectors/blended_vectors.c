/* Stratum — known-answer vectors for the legacy blended noise, from cubiomes.
 * Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
 *
 * cubiomes (MIT, Cubitect) is the reference SPEC §2 names. Its
 * `sampleSurfaceNoise` is the same computation `minecraft:old_blended_noise`
 * performs, and `octaveInit` seeds it the way a dimension declaring
 * `legacy_random_source` does.
 *
 * WHAT THIS CANNOT COVER, stated here because it is easy to read the
 * agreement below as more than it is:
 *
 *   * cubiomes models the pre-1.18 noise, which had no
 *     `smear_scale_multiplier`. These vectors therefore pin only the
 *     multiplier-of-one case; vanilla's own data uses 8.0 and 4.0.
 *   * cubiomes' `maintainPrecision` is a no-op — the real line is commented
 *     out in noise.h as "useless in practice" — so the coordinates below are
 *     kept small enough that vanilla's wrap would be the identity too.
 *     Where the wrap actually bites, nothing here has an opinion.
 *   * Nothing here says how a modern dimension seeds these octaves.
 *
 * cubiomes is fetched at a pinned commit by
 * tools/vectors/generate-blended-vectors.sh and is never vendored.
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

static const uint64_t kSeeds[] = {
    0ULL, 1ULL, 42ULL, (uint64_t)-1LL, 123456789ULL,
};
static const int kSeedCount = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

/* The scale sets vanilla's three dimensions use, plus the pre-1.18 overworld
 * pair cubiomes hardcodes — different enough that a formula that happened to
 * fit one would not fit the rest. */
struct Config {
    double xzScale, yScale, xzFactor, yFactor;
};

static const struct Config kConfigs[] = {
    {0.25, 0.125, 80.0, 160.0},                             /* overworld */
    {0.25, 0.375, 80.0, 60.0},                              /* nether */
    {0.25, 0.25, 80.0, 160.0},                              /* end */
    {0.9999999814507745, 0.9999999814507745, 80.0, 160.0},  /* pre-1.18 */
    {2.0, 1.0, 80.0, 160.0},                                /* pre-1.18 end */
};
static const int kConfigCount = (int)(sizeof(kConfigs) / sizeof(kConfigs[0]));

/* Deliberately far from the origin in places, but never so far that
 * x * 684.412 * xzScale approaches 2^24 — past that cubiomes and vanilla
 * part company, because cubiomes does not wrap. */
static const int kCoords[][3] = {
    {0, 0, 0},      {1, 0, 0},       {0, 64, 0},     {-1, -1, -1},
    {17, 33, -49},  {100, 0, -100},  {-256, 128, 256}, {1000, -60, -1000},
    {4, 8, 12},     {-7, 319, 5},
};
static const int kCoordCount = (int)(sizeof(kCoords) / sizeof(kCoords[0]));

/* The three stacks, seeded from one LCG in the order octaveInit imposes. */
static void initBlended(SurfaceNoise *sn, uint64_t seed, const struct Config *config) {
    uint64_t s;
    setSeed(&s, seed);
    octaveInit(&sn->octmin, &s, sn->oct + 0, -15, 16);
    octaveInit(&sn->octmax, &s, sn->oct + 16, -15, 16);
    octaveInit(&sn->octmain, &s, sn->oct + 32, -7, 8);
    sn->xzScale = config->xzScale;
    sn->yScale = config->yScale;
    sn->xzFactor = config->xzFactor;
    sn->yFactor = config->yFactor;
}

static void emitPerlinInit(void) {
    /* The LCG-seeded Perlin construction on its own, so that a failure in the
     * blended noise can be told apart from a failure in what seeds it. */
    printf("struct LegacyPerlinVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    int index;\n");
    printf("    std::uint64_t originX;\n");
    printf("    std::uint64_t originY;\n");
    printf("    std::uint64_t originZ;\n");
    printf("    std::array<std::uint8_t, 256> permutation;\n");
    printf("};\n\n");
    printf("constexpr auto kLegacyPerlinVectors = std::to_array<LegacyPerlinVector>({\n");

    for (int s = 0; s < kSeedCount; s++) {
        uint64_t state;
        setSeed(&state, kSeeds[s]);
        /* The first three drawn, and one from deep in the sequence: an
         * implementation that reset the generator between octaves would agree
         * on the first and not on the fortieth. */
        for (int i = 0; i < 40; i++) {
            PerlinNoise p;
            perlinInit(&p, &state);
            if (i != 0 && i != 1 && i != 2 && i != 39) {
                continue;
            }
            printf("    {INT64_C(%lld), %d, ", (long long)kSeeds[s], i);
            printDouble(p.a); printf(", ");
            printDouble(p.b); printf(", ");
            printDouble(p.c); printf(", {{");
            for (int k = 0; k < 256; k++) {
                printf("%s%d", k ? "," : "", (int)p.d[k]);
            }
            printf("}}},\n");
        }
    }
    printf("});\n\n");
}

static void emitSamples(void) {
    printf("struct BlendedVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    std::uint64_t xzScale;\n");
    printf("    std::uint64_t yScale;\n");
    printf("    std::uint64_t xzFactor;\n");
    printf("    std::uint64_t yFactor;\n");
    printf("    std::int32_t x;\n");
    printf("    std::int32_t y;\n");
    printf("    std::int32_t z;\n");
    printf("    std::uint64_t value;\n");
    printf("};\n\n");
    printf("constexpr auto kBlendedVectors = std::to_array<BlendedVector>({\n");

    for (int s = 0; s < kSeedCount; s++) {
        for (int c = 0; c < kConfigCount; c++) {
            SurfaceNoise sn;
            initBlended(&sn, kSeeds[s], &kConfigs[c]);
            for (int k = 0; k < kCoordCount; k++) {
                const double value =
                    sampleSurfaceNoise(&sn, kCoords[k][0], kCoords[k][1], kCoords[k][2]);
                printf("    {INT64_C(%lld), ", (long long)kSeeds[s]);
                printDouble(kConfigs[c].xzScale); printf(", ");
                printDouble(kConfigs[c].yScale); printf(", ");
                printDouble(kConfigs[c].xzFactor); printf(", ");
                printDouble(kConfigs[c].yFactor); printf(", ");
                printf("%d, %d, %d, ", kCoords[k][0], kCoords[k][1], kCoords[k][2]);
                printDouble(value); printf("},\n");
            }
        }
    }
    printf("});\n\n");
}

int main(void) {
    printf("// GENERATED FILE — DO NOT EDIT BY HAND.\n");
    printf("//\n");
    printf("// Known-answer vectors for stratum::noise::BlendedNoise, produced by\n");
    printf("// cubiomes (MIT), the noise reference SPEC §2 names. Doubles are raw bit\n");
    printf("// patterns. Regenerate with: tools/vectors/generate-blended-vectors.sh\n");
    printf("//\n");
    printf("// These pin the octave loop, the blend and the legacy seeding. They do NOT\n");
    printf("// cover smear_scale_multiplier, the coordinate wrap, or how a modern\n");
    printf("// dimension seeds these octaves — see the driver for why.\n");
    printf("\n");
    printf("#include <array>\n");
    printf("#include <cstdint>\n");
    printf("\n");
    printf("// clang-format off\n\n");

    emitPerlinInit();
    emitSamples();

    printf("// clang-format on\n");
    return 0;
}
