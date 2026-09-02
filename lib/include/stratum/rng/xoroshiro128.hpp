// Stratum — Xoroshiro128++ and vanilla's seed derivation.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §5.3 allows exactly two generators: this one and the Java LCG. Modern
// vanilla worldgen derives from Xoroshiro128++.
//
// The algorithm is by David Blackman and Sebastiano Vigna, published at
// https://prng.di.unimi.it/xoroshiro128plusplus.c and dedicated to the public
// domain. It is reimplemented here from that reference.
//
// WHAT IS AND IS NOT ESTABLISHED (CLAUDE.md: "verify RNG derivations against
// vectors before building on them"):
//
//   * The generator itself and mixStafford13 are checked against the JDK's
//     own jdk.internal.random.Xoroshiro128PlusPlus — seeded to an exact
//     state through its public (x0, x1) constructor — and against
//     RandomSupport.mixStafford13. Those are genuine independent oracles.
//   * upgradeSeedTo128Bit and the derived nextInt/nextDouble/nextFloat/
//     nextBoolean are implementations of the *documented* algorithms,
//     checked against an implementation that is not ours. That catches a
//     porting mistake. It does not prove Mojang composes the pieces this
//     way; only diffing generated terrain against the goldens does, which
//     needs the density-function pipeline (M3).
//
// nextInt(bound) and seedFromHashOf are checked against cubiomes (MIT), the
// noise and biome reference SPEC §2 names, and the MD5 halves it salts with
// are checked against md5sum. Still absent for want of an oracle:
// nextGaussian and fork(). They arrive with M3.

#pragma once

#include <stratum/hash/md5.hpp>

#include <bit>
#include <cassert>
#include <cstdint>
#include <string_view>

namespace stratum::rng {

/// Stafford's variant 13 of the splitmix64 finalizer, which vanilla uses to
/// derive generator state from a seed.
[[nodiscard]] constexpr std::uint64_t mixStafford13(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

/// The 128 bits of generator state.
struct Seed128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    [[nodiscard]] constexpr bool operator==(const Seed128& other) const noexcept = default;

    /// Xoroshiro cannot escape an all-zero state: it would emit nothing but
    /// zeroes forever. The upgrade below never produces one.
    [[nodiscard]] constexpr bool degenerate() const noexcept { return (lo | hi) == 0U; }
};

/// The 64-bit fractional parts of the silver and golden ratios, which
/// vanilla uses to spread a world seed across 128 bits.
inline constexpr std::uint64_t kSilverRatio64 = UINT64_C(0x6A09E667F3BCC909);
inline constexpr std::uint64_t kGoldenRatio64 = UINT64_C(0x9E3779B97F4A7C15);

/// Vanilla's 64-bit seed to 128-bit state upgrade.
[[nodiscard]] constexpr Seed128 upgradeSeedTo128Bit(std::int64_t seed) noexcept {
    const std::uint64_t lo = static_cast<std::uint64_t>(seed) ^ kSilverRatio64;
    // Wrapping addition, as Java's is.
    const std::uint64_t hi = lo + kGoldenRatio64;
    return Seed128{mixStafford13(lo), mixStafford13(hi)};
}

class Xoroshiro128PlusPlus {
public:
    /// From an exact state. Precondition: the state is not all zeroes.
    explicit constexpr Xoroshiro128PlusPlus(Seed128 state) noexcept : lo_(state.lo), hi_(state.hi) {
        assert(!state.degenerate());
    }

    /// From a world seed, through vanilla's documented upgrade.
    explicit constexpr Xoroshiro128PlusPlus(std::int64_t seed) noexcept
        : Xoroshiro128PlusPlus(upgradeSeedTo128Bit(seed)) {}

    constexpr std::int64_t nextLong() noexcept {
        const std::uint64_t s0 = lo_;
        std::uint64_t s1 = hi_;
        const std::uint64_t result = std::rotl(s0 + s1, 17) + s0;

        s1 ^= s0;
        lo_ = std::rotl(s0, 49) ^ s1 ^ (s1 << 21U);
        hi_ = std::rotl(s1, 28);

        return static_cast<std::int64_t>(result);
    }

    /// The top @p bits of the next draw. Precondition: 1 <= bits <= 32.
    constexpr std::int32_t next(int bits) noexcept {
        assert(bits >= 1 && bits <= 32);
        const auto drop = static_cast<unsigned>(64 - bits);
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(nextLong()) >> drop));
    }

    constexpr std::int32_t nextInt() noexcept { return next(32); }

    /// Uniform in [0, bound), by Lemire's multiply-and-shift with rejection:
    /// the low 32 bits of a draw are multiplied by the bound and the high
    /// half taken, retrying only for the values that would bias the result.
    /// Precondition: bound > 0.
    ///
    /// Note this is a different method from the Java LCG's bounded draw, and
    /// consumes a different number of draws. The two are not interchangeable.
    constexpr std::int32_t nextInt(std::int32_t bound) noexcept {
        assert(bound > 0);
        const auto limit = static_cast<std::uint32_t>(bound);

        auto scaled = lowHalf() * static_cast<std::uint64_t>(limit);
        if (static_cast<std::uint32_t>(scaled) < limit) {
            // (2^32 - bound) mod bound: the count of low products that would
            // over-represent the first few values.
            const std::uint32_t threshold = (~limit + 1U) % limit;
            while (static_cast<std::uint32_t>(scaled) < threshold) {
                scaled = lowHalf() * static_cast<std::uint64_t>(limit);
            }
        }
        return static_cast<std::int32_t>(scaled >> 32U);
    }

    constexpr bool nextBoolean() noexcept { return nextInt() < 0; }

    constexpr double nextDouble() noexcept {
        constexpr double kDoubleUnit = 0x1.0P-53;
        return static_cast<double>(static_cast<std::uint64_t>(nextLong()) >> 11U) * kDoubleUnit;
    }

    constexpr float nextFloat() noexcept {
        constexpr float kFloatUnit = 0x1.0P-24F;
        return static_cast<float>(static_cast<std::uint64_t>(nextLong()) >> 40U) * kFloatUnit;
    }

    [[nodiscard]] constexpr Seed128 state() const noexcept { return Seed128{lo_, hi_}; }

private:
    /// The low 32 bits of the next draw, which is what the bounded draw uses.
    constexpr std::uint64_t lowHalf() noexcept {
        return static_cast<std::uint64_t>(nextLong()) & UINT64_C(0xFFFFFFFF);
    }

    std::uint64_t lo_;
    std::uint64_t hi_;
};

/// The 128-bit salt vanilla derives from a name: the MD5 of the name, taken
/// as two big-endian halves. `lo` is the first eight bytes of the digest and
/// `hi` the last, matching how the halves are XORed into generator state.
[[nodiscard]] inline Seed128 seedFromHashOf(std::string_view name) noexcept {
    const hash::Md5Digest digest = hash::md5(name);
    Seed128 seed;
    for (std::size_t i = 0; i < 8; ++i) {
        seed.lo = (seed.lo << 8U) | digest[i];
        seed.hi = (seed.hi << 8U) | digest[i + 8U];
    }
    return seed;
}

} // namespace stratum::rng
