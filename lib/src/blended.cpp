// Stratum — the legacy "blended" 3D noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Adapted from cubiomes (https://github.com/Cubitect/cubiomes, MIT,
// Cubitect), read at e61f90580cbdd883214a8054670dacae655e59c0: the octave
// loop and blend in `sampleSurfaceNoise` (biomenoise.c); the order the three
// stacks are drawn in from one generator — sixteen minimum, sixteen maximum,
// eight blend — which is the opening of `initSurfaceNoise` (biomenoise.c);
// the seeding order within a stack, `octaveInit` (noise.c); and
// `maintainPrecision`, which is noise.h's function of that name with the
// body noise.h keeps as a commented-out line,
// `x - round(x / 33554432.0) * 33554432.0` — the name, the wrap into ±2^24
// and its period of 2^25. THIS PROJECT'S OWN: the half-up rounding,
// floor(v / 2^25 + 0.5), where C's `round` takes a half away from zero, and
// applying the wrap at all, where cubiomes' function returns its argument.
// The clean-room spec/blended-noise-spec.md (Q1.6), which arrived later,
// states the same wrap and rounding. deepslate (`modern`) and the server
// (`legacyFromWorldSeed`) later confirmed the order across the stacks; they
// are not its source. Recorded in NOTICE.

#include <stratum/noise/blended.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/noise/perlin_kernel.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

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

/// maintainPrecision's body, inline for BlendedNoise::sample's three calls
/// per octave.
///
/// Half up, floor(v / 2^25 + 0.5), not std::round, which takes a half away
/// from zero (spec Q1.6's reading; this project's own, NOTICE). The two part
/// where v / 2^25 is a negative half-integer, at v = 2^24 - 2^-29 — the
/// largest quotient below one half, which the added half rounds up to one —
/// at odd quotients of magnitude 2^52 to 2^53, and in the sign of a zero at
/// v = -0. NOTICE says so too.
///
/// floorExceptNegativeZero is std::floor here, to the bit: its argument is a
/// sum whose right operand is +0.5, which IEEE addition never takes to -0.0.
///
/// Inside +/-2^23, which is every coordinate within some 49 000 blocks of
/// the origin at vanilla's overworld scale, the expression is `value` itself
/// and is returned as such. That is the same double, not an approximation of
/// it: there `value / 2^25` lies within +/-1/4 (exact, or a subnormal
/// rounding of a tiny value, which stays inside that bound), so the quotient plus
/// a half rounds into [1/4, 3/4], whose floor is +0.0; +0.0 * 2^25 is +0.0;
/// and `value - (+0.0)` is `value` to the bit for every double, -0.0
/// included. NaN fails the comparison and takes the expression.
[[nodiscard]] STRATUM_NOISE_INLINE double wrapCoordinate(double value) noexcept {
    if (std::fabs(value) < 8388608.0) {
        return value;
    }
    return value -
           (detail::floorExceptNegativeZero((value / kPrecisionPeriod) + 0.5) * kPrecisionPeriod);
}

} // namespace

double maintainPrecision(double value) noexcept {
    return wrapCoordinate(value);
}

namespace {

/// The cap handed to the Perlin sampler for one octave.
///
/// @p slab is the octave's slab width, multiplier included; @p bareSlab is
/// the same width before the multiplier. Which of the two the cap uses, and
/// what happens below y = 0, is the whole difference between the two
/// readings — see BlendedNoise::Smear.
[[nodiscard]] double smearCap(BlendedNoise::Reading reading, double y, double slab,
                              double bareSlab) noexcept {
    if (reading == BlendedNoise::Reading::PreModern) {
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
    return build(random, parameters, Reading::PreModern);
}

BlendedNoise BlendedNoise::withModernReading(rng::JavaRandom& random, Parameters parameters) {
    return build(random, parameters, Reading::Modern);
}

BlendedNoise BlendedNoise::legacyFromWorldSeed(std::int64_t worldSeed, Parameters parameters) {
    // The world seed straight into the LCG. Written out rather than folded
    // into `build` because the absence of a fork and of a name salt is the
    // measured claim — every rival that lost to this one differed here and
    // nowhere else.
    rng::JavaRandom random{worldSeed};
    return build(random, parameters, Reading::Modern);
}

BlendedNoise BlendedNoise::modern(std::int64_t worldSeed, Parameters parameters) {
    // One generator for all three stacks, named. The name is not decoration:
    // it is MD5-salted into the seed, so `terrain` or `old_blended_noise`
    // give an unrelated world rather than a nearby one.
    rng::Xoroshiro128PlusPlus random =
        rng::XoroshiroPositionalFactory(worldSeed).fromHashOf("minecraft:terrain");

    BlendedNoise noise;
    noise.parameters_ = parameters;
    noise.reading_ = Reading::Modern;
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

BlendedNoise BlendedNoise::build(rng::JavaRandom& random, Parameters parameters, Reading reading) {
    BlendedNoise noise;
    noise.parameters_ = parameters;
    noise.reading_ = reading;

    // Drawn in this order from one generator: the two limits, then the
    // blend — initSurfaceNoise's order (header). Sequenced through separate
    // loops rather than interleaved, because which stack takes which draw is
    // the whole seeding.
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

    // The first octave is sampled at the full scale and contributes once;
    // each one after that halves the frequency and doubles the weight, which
    // is the reverse of the usual arrangement and is what gives this noise
    // its long wavelengths. Both factors are powers of two, so each octave's
    // are the same doubles whichever stack's loop computes them.
    //
    // The blend first: it decides which of the two limits the clamped lerp
    // below reads. Past either end of [0, 1] it reads one limit alone, and
    // the other is not sampled — every octave of every stack is a pure
    // function of the point, so skipping one changes nothing but the work.
    // Each stack still sums its own octaves in their own order, so each sum
    // is the double it always was.
    double blend = 0.0;
    double persistence = 1.0;
    double contribution = 1.0;
    for (std::size_t i = 0; i < kBlendOctaves; ++i) {
        const double blendX = wrapCoordinate(x * xzStep * persistence);
        const double blendY = wrapCoordinate(y * yStep * persistence);
        const double blendZ = wrapCoordinate(z * xzStep * persistence);
        const double blendBare = yStep * persistence;
        const double blendSmear = blendBare * parameters_.smearScaleMultiplier;
        const double blendCap = smearCap(reading_, y, blendSmear, blendBare);
        blend +=
            detail::samplePerlinFoldable(blend_[i], blendX, blendY, blendZ, blendSmear, blendCap) *
            contribution;
        persistence *= 0.5;
        contribution *= 2.0;
    }
    const double part = 0.5 + (0.05 * blend);
    // clampedLerp reads `from` (the minimum) unless part >= 1, and `to` (the
    // maximum) unless !(part > 0) — NaN included, which reads `from`.
    const bool readsMinimum = !(part >= 1.0);
    const bool readsMaximum = part > 0.0;

    double minimum = 0.0;
    double maximum = 0.0;
    persistence = 1.0;
    contribution = 1.0;
    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        const double limitX = wrapCoordinate(x * xzScale * persistence);
        const double limitY = wrapCoordinate(y * yScale * persistence);
        const double limitZ = wrapCoordinate(z * xzScale * persistence);
        // The slab the y coordinate is folded onto, and the cap on how much
        // of the local offset folds. Which is which is Smear's business.
        const double limitBare = yScale * persistence;
        const double limitSmear = limitBare * parameters_.smearScaleMultiplier;
        const double limitCap = smearCap(reading_, y, limitSmear, limitBare);

        if (readsMinimum) {
            minimum += detail::samplePerlinFoldable(minimum_[i], limitX, limitY, limitZ, limitSmear,
                                                    limitCap) *
                       contribution;
        }
        if (readsMaximum) {
            maximum += detail::samplePerlinFoldable(maximum_[i], limitX, limitY, limitZ, limitSmear,
                                                    limitCap) *
                       contribution;
        }

        persistence *= 0.5;
        contribution *= 2.0;
    }

    // 512 leaves the pre-1.18 density space, of order a hundred; 65536 is
    // the sum of the sixteen doubling amplitudes and lands in the order-one
    // space `sloped_cheese` adds its other term in. The two differ by exactly
    // 128, which is how the modern one was measured (SPEC §11).
    const double divisor = reading_ == Reading::PreModern ? 512.0 : 65536.0;
    return clampedLerp(part, minimum / divisor, maximum / divisor);
}

} // namespace stratum::noise
