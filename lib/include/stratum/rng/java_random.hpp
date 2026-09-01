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

#include <cassert>
#include <cstdint>

namespace stratum::rng {

/// A value-semantics reimplementation of java.util.Random.
///
/// Copying forks the stream, which is what worldgen wants: RNGs are derived
/// per (seed, position, salt) and never shared between threads.
///
/// Every method is bit-exact against a real JVM. nextGaussian is the one
/// documented gap — see the note below.
class JavaRandom {
public:
    /// The LCG constants published in java.util.Random's specification.
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask = (UINT64_C(1) << 48) - 1U;

    explicit constexpr JavaRandom(std::int64_t seed) noexcept : seed_(scramble(seed)) {}

    /// Reseeds the stream. java.util.Random also discards any
    /// half-generated Gaussian pair here; that state does not exist yet
    /// (see the note on Gaussians below) and must be reset when it does.
    constexpr void setSeed(std::int64_t seed) noexcept { seed_ = scramble(seed); }

    /// The raw generator step: advances the 48-bit state and returns the
    /// top @p bits of it. Callers outside this class rarely want this
    /// directly, but vanilla's seed derivations do.
    ///
    /// Precondition: 1 <= bits <= 32.
    constexpr std::int32_t next(int bits) noexcept {
        assert(bits >= 1 && bits <= 32);
        seed_ = ((seed_ * kMultiplier) + kAddend) & kMask;
        const unsigned drop = static_cast<unsigned>(48 - bits);
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

    // nextGaussian is deliberately ABSENT, not forgotten.
    //
    // java.util.Random's Gaussian uses StrictMath.log, which is fdlibm and
    // therefore identical on every JVM. glibc's std::log differs from it by
    // one ulp on some inputs: measured against the JVM vectors, 5 of 96
    // values came back +1 ulp on x86-64/glibc. That is a Tier-A parity
    // failure (SPEC §7), so shipping the method would plant a bug that only
    // shows up as a chunk seam much later.
    //
    // Implementing it needs an fdlibm-derived log in this repository, which
    // is a licensing decision recorded as open in SPEC §11. Until then the
    // method does not exist, so a caller that needs it fails to compile
    // rather than silently generating wrong terrain.

    /// The scrambled 48-bit state. Exposed for seed-derivation code and
    /// tests; it is the whole of this generator's state apart from the
    /// Gaussian cache.
    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return seed_; }

private:
    [[nodiscard]] static constexpr std::uint64_t scramble(std::int64_t seed) noexcept {
        return (static_cast<std::uint64_t>(seed) ^ kMultiplier) & kMask;
    }

    std::uint64_t seed_;
};

} // namespace stratum::rng
