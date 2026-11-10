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
//
// The Modern reading's arithmetic is NOT cubiomes' order any more: since
// pipeline engine v20 its doubles are formed as spec/blended-noise-spec.md's
// floating-point amendments write them (Forms, below), which the server
// confirms to the bit (SPEC §11, "old_blended_noise's floating-point form").
// The PreModern reading keeps cubiomes' order.

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
/// In the Modern reading this is spec/blended-noise-spec.md's Q2.1 cap, the
/// position's own y base times the octave's scale: `fy * s` for the main
/// sampler, `py * s` for the limits, @p yBase being fy or py. A negative cap
/// never binds (Q2.2: the fraction itself is folded), which the sampler
/// spells as an infinite one. The PreModern reading is cubiomes', where the
/// cap is the block y times the octave's slab with the multiplier in it.
[[nodiscard]] double octaveCap(BlendedNoise::Reading reading, double y, double yBase, double scale,
                               double slab) noexcept {
    if (reading == BlendedNoise::Reading::PreModern) {
        return y * slab;
    }
    const double cap = yBase * scale;
    if (!(cap >= 0.0)) {
        // Never binds: the sampler takes min(cap, localY) and localY is below
        // one, so the fold runs at full effect. Infinity rather than a large
        // number, because "does not bind" is the claim and a magic constant
        // would only be an approximation of it.
        return std::numeric_limits<double>::infinity();
    }
    return cap;
}

/// The arithmetic each reading forms its doubles in, fixed by the
/// parameters. Each octave's position argument is `wrap(base * s)` in both
/// readings, s the octave's scale (1, 1/2, 1/4, ...); what differs is how the
/// main sampler's bases are formed and how the slab (Q2.1's divisor) is.
///
/// Modern, as spec/blended-noise-spec.md's Q1.4 and Q2.1 floating-point
/// amendments state and tests/conformance/vanilla_blended_fpform_test.cpp
/// confirms against the server: `fx = (x * Mxz) / xz_factor` — the product
/// first, then the division — and `fy` likewise; the divisor
/// `((My * k) / y_factor) * s` for the main sampler and `(My * k) * s` for
/// the limits, k the smear multiplier.
///
/// PreModern, as cubiomes': `x * (Mxz / xz_factor)`, and the slab
/// `((My / y_factor) * s) * k`. The two part in the last bit for about one
/// position in eleven.
struct Forms {
    double xzScale;    // Mxz = 684.412 * xz_scale
    double yScale;     // My = 684.412 * y_scale
    double xzStep;     // Mxz / xz_factor (PreModern)
    double yStep;      // My / y_factor (PreModern)
    double mainSmear;  // (My * k) / y_factor (Modern)
    double limitSmear; // My * k (Modern)
};

[[nodiscard]] Forms formsOf(const BlendedNoise::Parameters& parameters) noexcept {
    const double xzScale = kBaseFrequency * parameters.xzScale;
    const double yScale = kBaseFrequency * parameters.yScale;
    const double limitSmear = yScale * parameters.smearScaleMultiplier;
    return Forms{.xzScale = xzScale,
                 .yScale = yScale,
                 .xzStep = xzScale / parameters.xzFactor,
                 .yStep = yScale / parameters.yFactor,
                 .mainSmear = limitSmear / parameters.yFactor,
                 .limitSmear = limitSmear};
}

/// The engine's clamped lerp (Q1.5's floating-point amendment): `a` below 0,
/// `b` above 1, and otherwise `a + t * (b - a)` — at exactly 1 too, which is
/// `b` only because the skipped minimum sum leaves `a` at 0.0. A NaN
/// selector takes the lerp, and so NaN.
[[nodiscard]] double engineClampedLerp(double t, double a, double b) noexcept {
    if (t < 0.0) {
        return a;
    }
    if (t > 1.0) {
        return b;
    }
    return a + (t * (b - a));
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
    const Forms f = formsOf(parameters_);
    const bool modern = reading_ == Reading::Modern;
    // The main sampler's bases: fx, fy, fz (Forms).
    const double mainX = modern ? (x * f.xzScale) / parameters_.xzFactor : x * f.xzStep;
    const double mainY = modern ? (y * f.yScale) / parameters_.yFactor : y * f.yStep;
    const double mainZ = modern ? (z * f.xzScale) / parameters_.xzFactor : z * f.xzStep;
    // The limits' bases: px, py, pz, the same in both readings.
    const double limitBaseX = x * f.xzScale;
    const double limitBaseY = y * f.yScale;
    const double limitBaseZ = z * f.xzScale;

    // The first octave is sampled at the full scale and contributes once;
    // each one after that halves the frequency and doubles the weight, which
    // is the reverse of the usual arrangement and is what gives this noise
    // its long wavelengths. Both factors are powers of two, so each octave's
    // are the same doubles whichever stack's loop computes them, and a term
    // times its contribution is exactly the spec's term divided by its scale.
    //
    // The blend first: it decides which of the two limits are summed at all.
    // Past either end of [0, 1] the clamped lerp reads one limit alone, and
    // the other is not sampled. That is not only work saved: at a selector
    // of exactly 1.0 the unsummed minimum stays 0.0, and the lerp then gives
    // the maximum exactly (Q1.5's amendment).
    double blend = 0.0;
    double scale = 1.0;
    double contribution = 1.0;
    for (std::size_t i = 0; i < kBlendOctaves; ++i) {
        const double blendX = detail::wrapCoordinate(mainX * scale);
        const double blendY = detail::wrapCoordinate(mainY * scale);
        const double blendZ = detail::wrapCoordinate(mainZ * scale);
        const double blendSmear =
            modern ? f.mainSmear * scale : (f.yStep * scale) * parameters_.smearScaleMultiplier;
        const double blendCap = octaveCap(reading_, y, mainY, scale, blendSmear);
        blend +=
            detail::samplePerlinFoldable(blend_[i], blendX, blendY, blendZ, blendSmear, blendCap) *
            contribution;
        scale *= 0.5;
        contribution *= 2.0;
    }
    // The selector: (main / 10 + 1) / 2 in the Modern reading, as the
    // amendment states and the server confirms; cubiomes' 0.5 + 0.05 * main
    // in the PreModern. They part in the last bit for about one point in
    // eighty.
    const double part = modern ? ((blend / 10.0) + 1.0) / 2.0 : 0.5 + (0.05 * blend);
    // Which limits are summed. Modern: the engine's flags, selector >= 1
    // skips the minimum and selector <= 0 the maximum (a NaN sums both).
    // PreModern: clampedLerp's reads, `from` unless part >= 1 and `to`
    // unless !(part > 0).
    const bool readsMinimum = !(part >= 1.0);
    const bool readsMaximum = modern ? !(part <= 0.0) : part > 0.0;

    double minimum = 0.0;
    double maximum = 0.0;
    scale = 1.0;
    contribution = 1.0;
    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        const double limitX = detail::wrapCoordinate(limitBaseX * scale);
        const double limitY = detail::wrapCoordinate(limitBaseY * scale);
        const double limitZ = detail::wrapCoordinate(limitBaseZ * scale);
        // The slab the y coordinate is folded onto (Q2.1's divisor), and the
        // cap on how much of the local offset folds.
        const double limitSmear =
            modern ? f.limitSmear * scale : (f.yScale * scale) * parameters_.smearScaleMultiplier;
        const double limitCap = octaveCap(reading_, y, limitBaseY, scale, limitSmear);

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

        scale *= 0.5;
        contribution *= 2.0;
    }

    // Both limits over 512, which leaves the pre-1.18 density space, of
    // order a hundred. The Modern reading then divides the lerp by 128 more,
    // landing in the order-one space `sloped_cheese` adds its other term in
    // (SPEC §11).
    if (modern) {
        return engineClampedLerp(part, minimum / 512.0, maximum / 512.0) / 128.0;
    }
    return clampedLerp(part, minimum / 512.0, maximum / 512.0);
}

void BlendedNoise::sampleColumn(const double x, const std::span<const double> ys, const double z,
                                const std::span<double> out) const noexcept {
    const std::size_t count = ys.size();
    assert(count <= kColumn && out.size() == count);
    if (count == 0) {
        return;
    }
    // sample()'s doubles lane by lane. Every factor below that is not a
    // lane's own — the forms, each octave's scale, contribution, x and z,
    // wrapped, and its slab — is the double sample() makes, made once; a
    // lane's y base, its wrap and its cap are its own. Only the first `count`
    // entries of each array are written or read (of the gathers, the first
    // minimumCount and maximumCount).
    const Forms f = formsOf(parameters_);
    const bool modern = reading_ == Reading::Modern;
    const double mainX = modern ? (x * f.xzScale) / parameters_.xzFactor : x * f.xzStep;
    const double mainZ = modern ? (z * f.xzScale) / parameters_.xzFactor : z * f.xzStep;
    const double limitBaseX = x * f.xzScale;
    const double limitBaseZ = z * f.xzScale;

    // Each lane's three sums, each from 0.0 as sample()'s, and its two y
    // bases (sample()'s mainY and limitBaseY).
    std::array<double, kColumn> blend;
    std::array<double, kColumn> minimum;
    std::array<double, kColumn> maximum;
    std::array<double, kColumn> mainY;
    std::array<double, kColumn> limitY;
    for (std::size_t k = 0; k < count; ++k) {
        blend[k] = 0.0;
        minimum[k] = 0.0;
        maximum[k] = 0.0;
        mainY[k] = modern ? (ys[k] * f.yScale) / parameters_.yFactor : ys[k] * f.yStep;
        limitY[k] = ys[k] * f.yScale;
    }
    double scale = 1.0;
    double contribution = 1.0;
    for (std::size_t i = 0; i < kBlendOctaves; ++i) {
        const double blendX = detail::wrapCoordinate(mainX * scale);
        const double blendZ = detail::wrapCoordinate(mainZ * scale);
        const double blendSmear =
            modern ? f.mainSmear * scale : (f.yStep * scale) * parameters_.smearScaleMultiplier;
        detail::samplePerlinFoldableColumn(
            blend_[i], blendX,
            [&mainY, scale](std::size_t k) noexcept {
                return detail::wrapCoordinate(mainY[k] * scale);
            },
            blendZ, blendSmear,
            [this, &ys, &mainY, scale, blendSmear](std::size_t k) noexcept {
                return octaveCap(reading_, ys[k], mainY[k], scale, blendSmear);
            },
            count,
            [&blend, contribution](std::size_t k, double term) noexcept {
                blend[k] += term * contribution;
            });
        scale *= 0.5;
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
        part[k] = modern ? ((blend[k] / 10.0) + 1.0) / 2.0 : 0.5 + (0.05 * blend[k]);
        if (!(part[k] >= 1.0)) {
            minimumLanes[minimumCount++] = k;
        }
        if (modern ? !(part[k] <= 0.0) : part[k] > 0.0) {
            maximumLanes[maximumCount++] = k;
        }
    }

    scale = 1.0;
    contribution = 1.0;
    // One limit's octave over the lanes that read it: each lane's y and cap,
    // sampled together, each term added to its own lane's sum.
    const auto limitOctave = [&](const PerlinNoise& octave,
                                 const std::array<std::size_t, kColumn>& lanes,
                                 const std::size_t lanesCount, std::array<double, kColumn>& sums,
                                 const double octaveX, const double octaveZ,
                                 const double limitSmear) {
        detail::samplePerlinFoldableColumn(
            octave, octaveX,
            [&](std::size_t j) noexcept {
                return detail::wrapCoordinate(limitY[lanes[j]] * scale);
            },
            octaveZ, limitSmear,
            [&](std::size_t j) noexcept {
                return octaveCap(reading_, ys[lanes[j]], limitY[lanes[j]], scale, limitSmear);
            },
            lanesCount,
            [&](std::size_t j, double term) noexcept { sums[lanes[j]] += term * contribution; });
    };
    for (std::size_t i = 0; i < kLimitOctaves; ++i) {
        const double octaveX = detail::wrapCoordinate(limitBaseX * scale);
        const double octaveZ = detail::wrapCoordinate(limitBaseZ * scale);
        const double limitSmear =
            modern ? f.limitSmear * scale : (f.yScale * scale) * parameters_.smearScaleMultiplier;
        if (minimumCount > 0) {
            limitOctave(minimum_[i], minimumLanes, minimumCount, minimum, octaveX, octaveZ,
                        limitSmear);
        }
        if (maximumCount > 0) {
            limitOctave(maximum_[i], maximumLanes, maximumCount, maximum, octaveX, octaveZ,
                        limitSmear);
        }
        scale *= 0.5;
        contribution *= 2.0;
    }

    for (std::size_t k = 0; k < count; ++k) {
        out[k] = modern ? engineClampedLerp(part[k], minimum[k] / 512.0, maximum[k] / 512.0) / 128.0
                        : clampedLerp(part[k], minimum[k] / 512.0, maximum[k] / 512.0);
    }
}

} // namespace stratum::noise
