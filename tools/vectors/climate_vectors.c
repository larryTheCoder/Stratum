/* Stratum — known-answer vectors for the 2D density pipeline, from cubiomes.
 * Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
 *
 * cubiomes (MIT, Cubitect) is the reference SPEC §2 names for noise and biome
 * math. tools/vectors/noise_vectors.c already checks the noise primitives
 * against it. This driver goes one layer up and checks the things built on
 * them: the seeding chain that turns a world seed into named noises, and the
 * overworld's 2D climate chain — shift_a/shift_b, flat_cache, shifted_noise
 * and the offset spline.
 *
 * WHAT THIS ESTABLISHES. cubiomes is an independent reimplementation, so
 * agreement is strong evidence that our pipeline is right, not proof that
 * either matches Mojang; the goldens settle that (M3). What it does prove
 * outright is that vanilla 1.21.11's own density function JSON — which the
 * conformance test loads from the fixtures — describes the same computation
 * cubiomes hardcodes.
 *
 * ON FLOATS. Vanilla's cubic splines are float throughout, and so are
 * cubiomes' knot locations, derivatives and values. cubiomes' getSpline
 * nonetheless routes its two interpolations through a *double* lerp helper
 * shared with the rest of the library, so it rounds to float once at the end
 * where vanilla rounds at every step. getSplineStrict below is the
 * float-throughout reading, and both are emitted: the strict one is what our
 * interpreter is held to, and the difference between them is reported by the
 * test rather than hidden.
 *
 * cubiomes is fetched at a pinned commit by
 * tools/vectors/generate-climate-vectors.sh and is never vendored into this
 * repository. Nothing Mojang-derived appears in the output: the spline knots
 * are cubiomes', and the sample coordinates are ours.
 */

#include "biomenoise.h"
#include "noise.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cubiomes defines getSpline in biomenoise.c but does not export it in the
 * header. Declared here rather than reached through sampleBiomeNoise, which
 * would only hand back the climate parameters truncated to four decimal
 * places — no use to a comparison that has to be bit-exact. */
float getSpline(const Spline *sp, const float *vals);

static void printDouble(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    printf("UINT64_C(0x%016llX)", (unsigned long long)bits);
}

static void printFloat(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    printf("UINT32_C(0x%08lX)", (unsigned long)bits);
}

/* The same seeds the noise vectors use, so a failure here can be compared
 * against a passing case there. */
static const uint64_t kSeeds[] = {
    0ULL, 1ULL, 42ULL, (uint64_t)-1LL, 123456789ULL, (uint64_t)-4172144997902289642LL,
};
static const int kSeedCount = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

/* ------------------------------------------------------------------------
 * The noises the overworld's climate is built from, by the name vanilla
 * addresses them with. cubiomes hardcodes the amplitudes and first octave;
 * our side reads them out of worldgen/noise/<name>.json, so agreement also
 * checks that we parsed those files as cubiomes' author read them.
 * --------------------------------------------------------------------- */
struct NamedClimate {
    int nptype;
    const char *id;
};

static const struct NamedClimate kNamedClimates[] = {
    {NP_SHIFT, "minecraft:offset"},
    {NP_TEMPERATURE, "minecraft:temperature"},
    {NP_HUMIDITY, "minecraft:vegetation"},
    {NP_CONTINENTALNESS, "minecraft:continentalness"},
    {NP_EROSION, "minecraft:erosion"},
    {NP_WEIRDNESS, "minecraft:ridge"},
};
static const int kNamedClimateCount =
    (int)(sizeof(kNamedClimates) / sizeof(kNamedClimates[0]));

/* Coordinates that exercise the lattice rather than the origin. */
static const double kSampleCoords[][3] = {
    {0.0, 0.0, 0.0},      {1.0, 0.0, 0.0},     {0.5, 0.5, 0.5},
    {-1.0, -1.0, -1.0},   {-0.5, 2.25, 13.75}, {123.456, 0.0, -78.9},
    {1000.5, 64.0, -1000.5},
};
static const int kSampleCoordCount =
    (int)(sizeof(kSampleCoords) / sizeof(kSampleCoords[0]));

static void emitNoiseSamples(void) {
    printf("struct ClimateNoiseVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    std::string_view noise;\n");
    printf("    std::uint64_t x;\n");
    printf("    std::uint64_t y;\n");
    printf("    std::uint64_t z;\n");
    printf("    std::uint64_t value;\n");
    printf("};\n\n");
    printf("constexpr auto kClimateNoiseVectors = std::to_array<ClimateNoiseVector>({\n");

    for (int s = 0; s < kSeedCount; s++) {
        BiomeNoise bn;
        initBiomeNoise(&bn, MC_1_21);
        setBiomeSeed(&bn, kSeeds[s], 0);

        for (int n = 0; n < kNamedClimateCount; n++) {
            const DoublePerlinNoise *dpn = &bn.climate[kNamedClimates[n].nptype];
            for (int c = 0; c < kSampleCoordCount; c++) {
                const double value = sampleDoublePerlin(
                    dpn, kSampleCoords[c][0], kSampleCoords[c][1], kSampleCoords[c][2]);
                printf("    {INT64_C(%lld), \"%s\", ", (long long)kSeeds[s],
                       kNamedClimates[n].id);
                printDouble(kSampleCoords[c][0]); printf(", ");
                printDouble(kSampleCoords[c][1]); printf(", ");
                printDouble(kSampleCoords[c][2]); printf(", ");
                printDouble(value); printf("},\n");
            }
        }
    }
    printf("});\n\n");
}

/* ------------------------------------------------------------------------
 * The offset spline, evaluated float-throughout as vanilla's CubicSpline is.
 * This is cubiomes' getSpline with its double lerp helper replaced; the
 * traversal, the knots and the extrapolation rule are cubiomes'.
 * --------------------------------------------------------------------- */
static float lerpF(float part, float from, float to) {
    return from + part * (to - from);
}

static float getSplineStrict(const Spline *sp, const float *vals) {
    if (!sp || sp->len <= 0 || sp->len >= 12) {
        fprintf(stderr, "getSplineStrict(): bad parameters\n");
        exit(1);
    }
    if (sp->len == 1) {
        return ((FixSpline *)sp)->val;
    }

    const float f = vals[sp->typ];
    int i;
    for (i = 0; i < sp->len; i++) {
        if (sp->loc[i] >= f) {
            break;
        }
    }
    if (i == 0 || i == sp->len) {
        if (i) {
            i--;
        }
        const float v = getSplineStrict(sp->val[i], vals);
        return v + sp->der[i] * (f - sp->loc[i]);
    }

    const float g = sp->loc[i - 1];
    const float h = sp->loc[i];
    const float k = (f - g) / (h - g);
    const float n = getSplineStrict(sp->val[i - 1], vals);
    const float o = getSplineStrict(sp->val[i], vals);
    const float p = sp->der[i - 1] * (h - g) - (o - n);
    const float q = -sp->der[i] * (h - g) + (o - n);
    return lerpF(k, n, o) + k * (1.0F - k) * lerpF(k, p, q);
}

/* Quarter positions, which is the resolution vanilla's climate is sampled
 * at and the one cubiomes works in. The conformance test multiplies these
 * by four to get block coordinates, and also samples off-corner blocks
 * inside the same cell — which is what makes flat_cache's relocation
 * observable. */
static const int kQuartCoords[][2] = {
    {0, 0},      {1, 0},        {0, 1},       {-1, -1},   {7, 13},
    {-7, 13},    {13, -7},      {-13, -7},    {100, 100}, {-100, 250},
    {1234, -567},{-4096, 4096}, {31, 31},     {-1, 0},    {0, -1},
};
static const int kQuartCoordCount = (int)(sizeof(kQuartCoords) / sizeof(kQuartCoords[0]));

static void emitClimateChain(void) {
    printf("struct ClimateChainVector {\n");
    printf("    std::int64_t seed;\n");
    printf("    std::int32_t quartX;\n");
    printf("    std::int32_t quartZ;\n");
    printf("    std::uint64_t shiftX;\n");
    printf("    std::uint64_t shiftZ;\n");
    printf("    std::uint64_t continents;\n");
    printf("    std::uint64_t erosion;\n");
    printf("    std::uint64_t ridges;\n");
    printf("    std::uint64_t ridgesFolded;\n");
    printf("    std::uint64_t offset;\n");
    printf("    std::uint32_t splineStrict;\n");
    printf("    std::uint32_t splineCubiomes;\n");
    printf("};\n\n");
    printf("constexpr auto kClimateChainVectors = std::to_array<ClimateChainVector>({\n");

    for (int s = 0; s < kSeedCount; s++) {
        BiomeNoise bn;
        initBiomeNoise(&bn, MC_1_21);
        setBiomeSeed(&bn, kSeeds[s], 0);

        for (int c = 0; c < kQuartCoordCount; c++) {
            const double qx = kQuartCoords[c][0];
            const double qz = kQuartCoords[c][1];

            /* shift_a and shift_b: one noise, sampled with the axes rotated. */
            const double shiftX = sampleDoublePerlin(&bn.climate[NP_SHIFT], qx, 0, qz) * 4.0;
            const double shiftZ = sampleDoublePerlin(&bn.climate[NP_SHIFT], qz, qx, 0) * 4.0;
            const double px = qx + shiftX;
            const double pz = qz + shiftZ;

            const double continents =
                sampleDoublePerlin(&bn.climate[NP_CONTINENTALNESS], px, 0, pz);
            const double erosion = sampleDoublePerlin(&bn.climate[NP_EROSION], px, 0, pz);
            const double ridges = sampleDoublePerlin(&bn.climate[NP_WEIRDNESS], px, 0, pz);

            /* ridges_folded, in double. cubiomes computes this in float from
             * an already-narrowed weirdness; vanilla's density function is a
             * mul/add/abs tree, so it is double until the spline narrows it.
             * Following vanilla here rather than cubiomes is deliberate:
             * otherwise the comparison would be testing cubiomes' float
             * pipeline rather than the spline. */
            const double ridgesFolded =
                -3.0 * (-0.3333333333333333 + fabs(-0.6666666666666666 + fabs(ridges)));

            const float vals[4] = {
                (float)continents, (float)erosion, (float)ridgesFolded, (float)ridges,
            };
            const float splineStrict = getSplineStrict(bn.sp, vals);
            const float splineCubiomes = getSpline(bn.sp, vals);

            /* What overworld/offset.json computes once blend_alpha is 1 and
             * blend_offset is 0, which is the no-blending state this engine
             * always generates in. */
            const double offset = -0.5037500262260437 + (double)splineStrict;

            printf("    {INT64_C(%lld), %d, %d, ", (long long)kSeeds[s], kQuartCoords[c][0],
                   kQuartCoords[c][1]);
            printDouble(shiftX); printf(", ");
            printDouble(shiftZ); printf(", ");
            printDouble(continents); printf(", ");
            printDouble(erosion); printf(", ");
            printDouble(ridges); printf(", ");
            printDouble(ridgesFolded); printf(", ");
            printDouble(offset); printf(", ");
            printFloat(splineStrict); printf(", ");
            printFloat(splineCubiomes); printf("},\n");
        }
    }
    printf("});\n\n");
}

int main(void) {
    printf("// GENERATED FILE — DO NOT EDIT BY HAND.\n");
    printf("//\n");
    printf("// Known-answer vectors for the 2D density pipeline, produced by cubiomes\n");
    printf("// (MIT), the noise and biome reference SPEC §2 names. Doubles and floats\n");
    printf("// are raw bit patterns: parity here is bit-exact or it is nothing.\n");
    printf("// Regenerate with: tools/vectors/generate-climate-vectors.sh\n");
    printf("//\n");
    printf("// cubiomes is an independent reimplementation, so agreement with it is\n");
    printf("// strong evidence, not proof that either matches Mojang. That is settled\n");
    printf("// by diffing generated terrain against the goldens (M3).\n");
    printf("\n");
    printf("#include <array>\n");
    printf("#include <cstdint>\n");
    printf("#include <string_view>\n");
    printf("\n");
    printf("// clang-format off\n\n");

    emitNoiseSamples();
    emitClimateChain();

    printf("// clang-format on\n");
    return 0;
}
