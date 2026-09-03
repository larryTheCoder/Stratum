// Stratum — the legacy "blended" 3D noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Adapted from cubiomes (https://github.com/Cubitect/cubiomes, MIT,
// Cubitect): the octave loop and blend in `sampleSurfaceNoise` (biomenoise.c)
// and the seeding order in `octaveInit` (noise.c). Recorded in NOTICE.

#include <stratum/noise/blended.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

namespace stratum::noise {

namespace {

/// Vanilla's base frequency for this noise. Every scale in the JSON is a
/// multiple of it, which is why `xz_scale: 0.25` is not a quarter of a block.
constexpr double kBaseFrequency = 684.412;

/// 2^25. Coordinates are folded back into ±half of this.
constexpr double kPrecisionPeriod = 33554432.0;

/// Clamped at both ends rather than extrapolating: past either end the blend
/// is the limit itself, which is what stops the two limit noises crossing.
[[nodiscard]] double clampedLerp(double part, double from, double to) noexcept {
    if (!(part > 0.0)) {
        return from;
    }
    if (part >= 1.0) {
        return to;
    }
    return from + (part * (to - from));
}

} // namespace

double maintainPrecision(double value) noexcept {
    // Java rounds half up rather than away from zero, so this is floor(v+0.5)
    // and not std::round. The two differ only at exact half-integers, and
    // only ever at coordinates 2^25 apart, but "only" is not "never".
    return value - (std::floor((value / kPrecisionPeriod) + 0.5) * kPrecisionPeriod);
}

namespace {

/// The cap handed to the Perlin sampler for one octave.
///
/// @p slab is the octave's slab width, multiplier included; @p bareSlab is
/// the same width before the multiplier. Which of the two the cap uses, and
/// what happens below y = 0, is the whole difference between the two
/// readings — see BlendedNoise::Smear.
[[nodiscard]] double smearCap(BlendedNoise::Smear smear, double y, double slab,
                              double bareSlab) noexcept {
    if (smear == BlendedNoise::Smear::PreModern) {
        return y * slab;
    }
    if (y < 0.0) {
        // Never binds: the sampler takes min(cap, localY) and localY is below
        // one, so the fold runs at full effect. Infinity rather than a large
        // number, because "does not bind" is the claim and a magic constant
        // would only be an approximation of it.
        return std::numeric_limits<double>::infinity();
    }
    return y * bareSlab;
}

} // namespace

BlendedNoise BlendedNoise::legacy(rng::JavaRandom& random, Parameters parameters) {
    return build(random, parameters, Smear::PreModern);
}

BlendedNoise BlendedNoise::withMeasuredSmear(rng::JavaRandom& random, Parameters parameters) {
    return build(random, parameters, Smear::Measured);
}

BlendedNoise BlendedNoise::build(rng::JavaRandom& random, Parameters parameters, Smear smear) {
    BlendedNoise noise;
    noise.parameters_ = parameters;
    noise.smear_ = smear;

    // Drawn in this order from one generator: the two limits, then the
    // blend. Sequenced through separate loops rather than interleaved,
    // because which stack takes which draw is the whole seeding.
    noise.minimum_.reserve(kLimitOctaves);
    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        noise.minimum_.push_back(PerlinNoise::fromRandom(random));
    }
    noise.maximum_.reserve(kLimitOctaves);
    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        noise.maximum_.push_back(PerlinNoise::fromRandom(random));
    }
    noise.blend_.reserve(kBlendOctaves);
    for (std::size_t i = 0; i < kBlendOctaves; ++i) {
        noise.blend_.push_back(PerlinNoise::fromRandom(random));
    }

    return noise;
}

double BlendedNoise::sample(double x, double y, double z) const noexcept {
    const double xzScale = kBaseFrequency * parameters_.xzScale;
    const double yScale = kBaseFrequency * parameters_.yScale;
    const double xzStep = xzScale / parameters_.xzFactor;
    const double yStep = yScale / parameters_.yFactor;

    double minimum = 0.0;
    double maximum = 0.0;
    double blend = 0.0;

    // The first octave is sampled at the full scale and contributes once;
    // each one after that halves the frequency and doubles the weight, which
    // is the reverse of the usual arrangement and is what gives this noise
    // its long wavelengths.
    double persistence = 1.0;
    double contribution = 1.0;

    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        const double limitX = maintainPrecision(x * xzScale * persistence);
        const double limitY = maintainPrecision(y * yScale * persistence);
        const double limitZ = maintainPrecision(z * xzScale * persistence);
        // The slab the y coordinate is folded onto, and the cap on how much
        // of the local offset folds. Which is which is Smear's business.
        const double limitBare = yScale * persistence;
        const double limitSmear = limitBare * parameters_.smearScaleMultiplier;
        const double limitCap = smearCap(smear_, y, limitSmear, limitBare);

        minimum += minimum_[i].sample(limitX, limitY, limitZ, limitSmear, limitCap) * contribution;
        maximum += maximum_[i].sample(limitX, limitY, limitZ, limitSmear, limitCap) * contribution;

        if (i < kBlendOctaves) {
            const double blendX = maintainPrecision(x * xzStep * persistence);
            const double blendY = maintainPrecision(y * yStep * persistence);
            const double blendZ = maintainPrecision(z * xzStep * persistence);
            const double blendBare = yStep * persistence;
            const double blendSmear = blendBare * parameters_.smearScaleMultiplier;
            const double blendCap = smearCap(smear_, y, blendSmear, blendBare);
            blend += blend_[i].sample(blendX, blendY, blendZ, blendSmear, blendCap) * contribution;
        }

        persistence *= 0.5;
        contribution *= 2.0;
    }

    return clampedLerp(0.5 + (0.05 * blend), minimum / 512.0, maximum / 512.0);
}

} // namespace stratum::noise
