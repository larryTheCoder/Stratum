// Stratum — the decoration stage's random source.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// What the server's features and placement modifiers draw from, MEASURED
// (SPEC §11, "The decoration framework, measured"): java.util.Random's
// derivations — `nextInt(bound)` by the power-of-two product or the modulo
// with its rejection, `nextLong` from two 32-bit halves, `nextDouble` from 26
// and 27 bits, a cached pair of gaussians — over a Xoroshiro128++ stream,
// each `next(bits)` the top bits of one of its longs. Not the Xoroshiro
// source's own `nextInt(bound)`, which the noises' positional sources use:
// that reading placed none of 3456 markers, this one all of them
// (tools/analysis/feature-probe.sh --variant seed).
//
// ADAPTED (CLAUDE.md, NOTICE) from cubiomes (https://github.com/Cubitect/
// cubiomes, MIT License, Copyright (c) 2020 Cubitect), at
// e61f90580cbdd883214a8054670dacae655e59c0: finders.c `getPopulationSeed`
// (its 1.18+ branch) with rng.h `xNextLongJ` and `xNextIntJ`, and the
// `population + salt` seeding its feature finders use. Rewritten in wrapping
// 64-bit arithmetic through javamath, and confirmed against the server:
//
//   population(world, x, z) = (x * a + z * b) ^ world
//       a = nextLong() | 1, b = nextLong() | 1, of a source seeded `world`;
//       x, z the chunk's lowest block corner
//   feature(population, index, step) = population + index + 10000 * step
//
// all in wrapping 64-bit arithmetic.
#pragma once

#include <stratum/javamath.hpp>
#include <stratum/math/fdlibm.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>

namespace stratum::rng {

class WorldgenRandom {
public:
    explicit constexpr WorldgenRandom(std::int64_t seed) noexcept : source_(seed) {}

    /// The top @p bits of the stream's next long. Precondition: 1..32.
    constexpr std::int32_t next(int bits) noexcept { return source_.next(bits); }

    constexpr std::int32_t nextInt() noexcept { return next(32); }

    /// Uniform in [0, bound). Precondition: bound > 0.
    constexpr std::int32_t nextInt(std::int32_t bound) noexcept {
        assert(bound > 0);
        const std::int32_t mask = bound - 1;
        if ((bound & mask) == 0) {
            const std::int64_t scaled = static_cast<std::int64_t>(bound) * next(31);
            return static_cast<std::int32_t>(javamath::shr(scaled, 31));
        }
        std::int32_t draw = next(31);
        std::int32_t result = draw % bound;
        while (javamath::wrappingAdd(javamath::wrappingSub(draw, result), mask) < 0) {
            draw = next(31);
            result = draw % bound;
        }
        return result;
    }

    /// Uniform in [@p minInclusive, @p maxInclusive]. Precondition: min <= max
    /// and the span fits an int32.
    constexpr std::int32_t nextIntBetween(std::int32_t minInclusive,
                                          std::int32_t maxInclusive) noexcept {
        return minInclusive + nextInt(maxInclusive - minInclusive + 1);
    }

    constexpr std::int64_t nextLong() noexcept {
        const std::int64_t high = next(32);
        const std::int64_t low = next(32);
        return javamath::wrappingAdd(javamath::shl(high, 32), low);
    }

    constexpr bool nextBoolean() noexcept { return next(1) != 0; }

    constexpr float nextFloat() noexcept {
        constexpr float kFloatUnit = 1.0F / static_cast<float>(1 << 24);
        return static_cast<float>(next(24)) * kFloatUnit;
    }

    constexpr double nextDouble() noexcept {
        constexpr double kDoubleUnit = 0x1.0P-53;
        const std::int64_t high = next(26);
        const std::int64_t low = next(27);
        return static_cast<double>(javamath::wrappingAdd(javamath::shl(high, 27), low)) *
               kDoubleUnit;
    }

    /// The polar method's pair, the second kept for the next call: measured
    /// on two consecutive draws of one feature (feature-probe.sh --variant
    /// terrain, the `clamped_normal` offsets).
    double nextGaussian() {
        if (haveNextGaussian_) {
            haveNextGaussian_ = false;
            return nextNextGaussian_;
        }
        double v1 = 0.0;
        double v2 = 0.0;
        double s = 0.0;
        do {
            v1 = (2.0 * nextDouble()) - 1.0;
            v2 = (2.0 * nextDouble()) - 1.0;
            s = (v1 * v1) + (v2 * v2);
        } while (s >= 1.0 || s <= 0.0);
        const double multiplier = std::sqrt((-2.0 * fdlibm::log(s)) / s);
        nextNextGaussian_ = v2 * multiplier;
        haveNextGaussian_ = true;
        return v1 * multiplier;
    }

    /// The stream advanced by @p longs draws, whatever they would have been.
    constexpr void skip(std::int32_t longs) noexcept {
        for (std::int32_t i = 0; i < longs; ++i) {
            static_cast<void>(source_.nextLong());
        }
    }

private:
    Xoroshiro128PlusPlus source_;
    double nextNextGaussian_ = 0.0;
    bool haveNextGaussian_ = false;
};

/// The seed every feature seed of the chunk whose lowest block corner is
/// (@p blockX, @p blockZ) is offset from.
[[nodiscard]] constexpr std::int64_t populationSeed(std::int64_t worldSeed, std::int32_t blockX,
                                                    std::int32_t blockZ) noexcept {
    WorldgenRandom random(worldSeed);
    const std::int64_t a = random.nextLong() | 1;
    const std::int64_t b = random.nextLong() | 1;
    const std::int64_t mixed =
        javamath::wrappingAdd(javamath::wrappingMul(std::int64_t{blockX}, a),
                              javamath::wrappingMul(std::int64_t{blockZ}, b));
    return mixed ^ worldSeed;
}

/// The seed of the feature at @p index of generation step @p step.
[[nodiscard]] constexpr std::int64_t featureSeed(std::int64_t population, std::int32_t index,
                                                 std::int32_t step) noexcept {
    return javamath::wrappingAdd(javamath::wrappingAdd(population, std::int64_t{index}),
                                 javamath::wrappingMul(std::int64_t{10000}, std::int64_t{step}));
}

} // namespace stratum::rng
