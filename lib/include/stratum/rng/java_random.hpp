// Stratum — java.util.Random-compatible linear congruential generator.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §5.3 allows exactly two generators: this one and Xoroshiro128++.
// Vanilla's legacy worldgen paths derive from java.util.Random, so a single
// wrong step here shifts every downstream value; the implementation is
// checked against known-answer vectors observed from a real JVM
// (tools/vectors/JavaRandomVectors.java).
//
// Two C++/Java divergences are handled explicitly below and must not be
// "simplified" away:
//
//   * Java specifies left-to-right evaluation of operands, so
//     `(next(32) << 32) + next(32)` has a defined call order. The order of
//     evaluation of `+`'s operands is unspecified in C++, so every such
//     expression here sequences its draws through named locals.
//   * Java's integer arithmetic wraps, so the rejection test inside
//     nextInt(bound) relies on overflow. It is spelled with the explicit
//     wrapping helpers rather than raw operators.

#pragma once

#include <stratum/javamath.hpp>
#include <stratum/math/fdlibm.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>

namespace stratum::rng {

/// A value-semantics reimplementation of java.util.Random.
///
/// Copying forks the stream, which is what worldgen wants: RNGs are derived
/// per (seed, position, salt) and never shared between threads.
///
/// Every method is bit-exact against a real JVM.
class JavaRandom {
public:
    /// The LCG constants published in java.util.Random's specification.
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask = (UINT64_C(1) << 48) - 1U;

    explicit constexpr JavaRandom(std::int64_t seed) noexcept : seed_(scramble(seed)) {}

    /// Reseeds the stream and, as java.util.Random specifies, discards any
    /// half-generated Gaussian pair.
    constexpr void setSeed(std::int64_t seed) noexcept {
        seed_ = scramble(seed);
        nextNextGaussian_ = 0.0;
        haveNextGaussian_ = false;
    }

    /// The raw generator step: advances the 48-bit state and returns the
    /// top @p bits of it. Callers outside this class rarely want this
    /// directly, but vanilla's seed derivations do.
    ///
    /// Precondition: 1 <= bits <= 32.
    constexpr std::int32_t next(int bits) noexcept {
        assert(bits >= 1 && bits <= 32);
        seed_ = ((seed_ * kMultiplier) + kAddend) & kMask;
        const auto drop = static_cast<unsigned>(48 - bits);
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(seed_ >> drop));
    }

    constexpr std::int32_t nextInt() noexcept { return next(32); }

    /// Uniform in [0, bound). Precondition: bound > 0.
    constexpr std::int32_t nextInt(std::int32_t bound) noexcept {
        assert(bound > 0);
        const std::int32_t mask = bound - 1;

        // Powers of two take a multiplicative fast path instead of a
        // remainder, and consume exactly one draw.
        if ((bound & mask) == 0) {
            const std::int64_t scaled = static_cast<std::int64_t>(bound) * next(31);
            return static_cast<std::int32_t>(javamath::shr(scaled, 31));
        }

        // Otherwise reject the values that would bias the modulo. The test
        // is an overflow check: it fails exactly when u - r + mask wraps.
        std::int32_t draw = next(31);
        std::int32_t result = draw % bound;
        while (javamath::wrappingAdd(javamath::wrappingSub(draw, result), mask) < 0) {
            draw = next(31);
            result = draw % bound;
        }
        return result;
    }

    constexpr std::int64_t nextLong() noexcept {
        // Sequenced: Java evaluates the high word first.
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
        // Sequenced: Java evaluates the high word first.
        const std::int64_t high = next(26);
        const std::int64_t low = next(27);
        return static_cast<double>(javamath::wrappingAdd(javamath::shl(high, 27), low)) *
               kDoubleUnit;
    }

    /// Marsaglia polar method, as java.util.Random specifies, including the
    /// cached second value of each generated pair.
    ///
    /// fdlibm::log, not std::log: Java uses StrictMath here, and a host libm
    /// is within an ulp of it rather than equal to it — measured, glibc
    /// disagrees with StrictMath on 30 of the 1057 log vectors. std::sqrt is
    /// fine, being IEEE-754 correctly rounded and therefore identical to
    /// StrictMath.sqrt.
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
            // s is a sum of squares of finite values, so s >= 0 always and
            // `s <= 0.0` is exactly Java's `s == 0` without tripping
            // -Wfloat-equal.
        } while (s >= 1.0 || s <= 0.0);

        const double multiplier = std::sqrt((-2.0 * fdlibm::log(s)) / s);
        nextNextGaussian_ = v2 * multiplier;
        haveNextGaussian_ = true;
        return v1 * multiplier;
    }

    /// The scrambled 48-bit state. Exposed for seed-derivation code and
    /// tests; it is the whole of this generator's state apart from the
    /// Gaussian cache.
    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return seed_; }

private:
    [[nodiscard]] static constexpr std::uint64_t scramble(std::int64_t seed) noexcept {
        return (static_cast<std::uint64_t>(seed) ^ kMultiplier) & kMask;
    }

    std::uint64_t seed_;
    double nextNextGaussian_ = 0.0;
    bool haveNextGaussian_ = false;
};

} // namespace stratum::rng
