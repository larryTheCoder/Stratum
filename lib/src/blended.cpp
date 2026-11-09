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

#include "perlin_kernel.hpp"

#include <stratum/noise/blended.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace stratum::noise {

namespace {

/// Vanilla's base frequency for this noise. Every scale in the JSON is a
/// multiple of it, which is why `xz_scale: 0.25` is not a quarter of a block.
constexpr double kBaseFrequency = 684.412;

/// 2^25. Coordinates are folded back into ±half of this.

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
    return detail::wrapCoordinate(value);
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
        const double blendX = detail::wrapCoordinate(x * xzStep * persistence);
        const double blendY = detail::wrapCoordinate(y * yStep * persistence);
        const double blendZ = detail::wrapCoordinate(z * xzStep * persistence);
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
        const double limitX = detail::wrapCoordinate(x * xzScale * persistence);
        const double limitY = detail::wrapCoordinate(y * yScale * persistence);
        const double limitZ = detail::wrapCoordinate(z * xzScale * persistence);
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

void BlendedNoise::sampleColumn(const double x, const std::span<const double> ys, const double z,
                                const std::span<double> out) const noexcept {
    const std::size_t count = ys.size();
    assert(count <= kColumn && out.size() == count);
    if (count == 0) {
        return;
    }
    // sample()'s doubles lane by lane. Every factor below that is not a
    // lane's own — the scales and steps, each octave's persistence,
    // contribution, x and z, wrapped, and its slab — is the double sample()
    // makes, made once; a lane's y, its wrap and its cap are its own. Only
    // the first `count` entries of each array are written or read (of the
    // gathers, the first minimumCount and maximumCount).
    const double xzScale = kBaseFrequency * parameters_.xzScale;
    const double yScale = kBaseFrequency * parameters_.yScale;
    const double xzStep = xzScale / parameters_.xzFactor;
    const double yStep = yScale / parameters_.yFactor;

    // Each lane's three sums, each from 0.0 as sample()'s.
    std::array<double, kColumn> blend;
    std::array<double, kColumn> minimum;
    std::array<double, kColumn> maximum;
    for (std::size_t k = 0; k < count; ++k) {
        blend[k] = 0.0;
        minimum[k] = 0.0;
        maximum[k] = 0.0;
    }
    double persistence = 1.0;
    double contribution = 1.0;
    for (std::size_t i = 0; i < kBlendOctaves; ++i) {
        const double blendX = detail::wrapCoordinate(x * xzStep * persistence);
        const double blendZ = detail::wrapCoordinate(z * xzStep * persistence);
        const double blendBare = yStep * persistence;
        const double blendSmear = blendBare * parameters_.smearScaleMultiplier;
        detail::samplePerlinFoldableColumn(
            blend_[i], blendX,
            [&ys, yStep, persistence](std::size_t k) noexcept {
                return detail::wrapCoordinate(ys[k] * yStep * persistence);
            },
            blendZ, blendSmear,
            [this, &ys, blendSmear, blendBare](std::size_t k) noexcept {
                return smearCap(reading_, ys[k], blendSmear, blendBare);
            },
            count,
            [&blend, contribution](std::size_t k, double term) noexcept {
                blend[k] += term * contribution;
            });
        persistence *= 0.5;
        contribution *= 2.0;
    }

    // Which lanes read each limit (sample()'s readsMinimum, readsMaximum),
    // and those lanes in order: a limit is sampled only for a lane that
    // reads it, as sample() samples it only for a point that does.
    std::array<double, kColumn> part;
    std::array<std::size_t, kColumn> minimumLanes;
    std::array<std::size_t, kColumn> maximumLanes;
    std::size_t minimumCount = 0;
    std::size_t maximumCount = 0;
    for (std::size_t k = 0; k < count; ++k) {
        part[k] = 0.5 + (0.05 * blend[k]);
        if (!(part[k] >= 1.0)) {
            minimumLanes[minimumCount++] = k;
        }
        if (part[k] > 0.0) {
            maximumLanes[maximumCount++] = k;
        }
    }

    persistence = 1.0;
    contribution = 1.0;
    // One limit's octave over the lanes that read it: each lane's y and cap,
    // sampled together, each term added to its own lane's sum.
    const auto limitOctave = [&](const PerlinNoise& octave,
                                 const std::array<std::size_t, kColumn>& lanes,
                                 const std::size_t lanesCount, std::array<double, kColumn>& sums,
                                 const double limitX, const double limitZ, const double limitSmear,
                                 const double limitBare) {
        detail::samplePerlinFoldableColumn(
            octave, limitX,
            [&](std::size_t j) noexcept {
                return detail::wrapCoordinate(ys[lanes[j]] * yScale * persistence);
            },
            limitZ, limitSmear,
            [&](std::size_t j) noexcept {
                return smearCap(reading_, ys[lanes[j]], limitSmear, limitBare);
            },
            lanesCount,
            [&](std::size_t j, double term) noexcept { sums[lanes[j]] += term * contribution; });
    };
    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        const double limitX = detail::wrapCoordinate(x * xzScale * persistence);
        const double limitZ = detail::wrapCoordinate(z * xzScale * persistence);
        const double limitBare = yScale * persistence;
        const double limitSmear = limitBare * parameters_.smearScaleMultiplier;
        if (minimumCount > 0) {
            limitOctave(minimum_[i], minimumLanes, minimumCount, minimum, limitX, limitZ,
                        limitSmear, limitBare);
        }
        if (maximumCount > 0) {
            limitOctave(maximum_[i], maximumLanes, maximumCount, maximum, limitX, limitZ,
                        limitSmear, limitBare);
        }
        persistence *= 0.5;
        contribution *= 2.0;
    }

    const double divisor = reading_ == Reading::PreModern ? 512.0 : 65536.0;
    for (std::size_t k = 0; k < count; ++k) {
        out[k] = clampedLerp(part[k], minimum[k] / divisor, maximum[k] / divisor);
    }
}

} // namespace stratum::noise
