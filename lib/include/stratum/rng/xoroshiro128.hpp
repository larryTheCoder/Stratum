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
// nextInt(bound), seedFromHashOf and the positional factory below are
// checked against cubiomes (MIT), the noise and biome reference SPEC §2
// names, and the MD5 halves they salt with are checked against md5sum. Still
// absent for want of an oracle: nextGaussian and the general-purpose fork()
// that derives a child generator for a sub-task — a different operation from
// the positional factory, and one nothing here reaches yet. They arrive with
// M3.

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
    return Seed128{.lo = mixStafford13(lo), .hi = mixStafford13(hi)};
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

    [[nodiscard]] constexpr Seed128 state() const noexcept { return Seed128{.lo = lo_, .hi = hi_}; }

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

/// Vanilla's positional random factory. A world seed is forked once into a
/// 128-bit base, and every named object — each noise here, each feature
/// later — derives its own generator by XORing the MD5 of its identifier
/// into that base. That is what makes two noises with different names
/// independent of each other while both stay a pure function of the world
/// seed, and what makes adding a noise leave the others undisturbed.
///
/// Checked against cubiomes (MIT), whose setBiomeSeed derives the climate
/// noises exactly this way: two draws from the world seed, then the MD5
/// halves of the noise's identifier XORed into them. cubiomes is an
/// independent reimplementation, so agreement is strong evidence rather than
/// proof that Mojang composes it so; the goldens settle that (M3).
class XoroshiroPositionalFactory {
public:
    explicit XoroshiroPositionalFactory(std::int64_t worldSeed) noexcept {
        Xoroshiro128PlusPlus source{worldSeed};
        // Sequenced through named locals rather than a braced initialiser:
        // both draws come from one generator, so which is taken first is
        // part of the answer, and C++ does not fix that order everywhere.
        const auto lo = static_cast<std::uint64_t>(source.nextLong());
        const auto hi = static_cast<std::uint64_t>(source.nextLong());
        base_ = Seed128{.lo = lo, .hi = hi};
    }

    /// The generator for one named object.
    [[nodiscard]] Xoroshiro128PlusPlus fromHashOf(std::string_view name) const noexcept {
        const Seed128 salt = seedFromHashOf(name);
        return Xoroshiro128PlusPlus{Seed128{.lo = base_.lo ^ salt.lo, .hi = base_.hi ^ salt.hi}};
    }

    [[nodiscard]] constexpr Seed128 base() const noexcept { return base_; }

private:
    Seed128 base_;
};

/// Vanilla's position-to-seed mix, shared by everything that wants a value per
/// block rather than per world.
///
/// Recovered by scoring candidates against server output, twice over and
/// independently: it is what places the aquifer's cell centres and what drives
/// surface rules' `vertical_gradient`. Shifting by 15 or 17 instead of 16, or
/// using a logical shift, collapses either match rate to chance.
///
/// Every step wraps, and the x term is multiplied as a 32-bit value before
/// being sign-extended. Spelled in unsigned arithmetic because C++ signed
/// overflow is undefined where Java's simply wraps.
[[nodiscard]] std::int64_t positionSeed(std::int32_t x, std::int32_t y, std::int32_t z) noexcept;

/// A named object's per-position generator.
///
/// The derivation is two forks around one salt: fork the world seed, XOR in
/// the MD5 of the name, then fork AGAIN. That second fork is the part that
/// nineteen earlier attempts at `vertical_gradient` missed — with a single
/// fork the same code scores 34% where the correct one scores 100%.
class PositionalSource {
public:
    explicit constexpr PositionalSource(Seed128 base) noexcept : base_(base) {}

    /// The generator for one block position. Cheap: one mix and one seeding.
    [[nodiscard]] Xoroshiro128PlusPlus at(std::int32_t x, std::int32_t y,
                                          std::int32_t z) const noexcept {
        const auto mixed = static_cast<std::uint64_t>(positionSeed(x, y, z));
        return Xoroshiro128PlusPlus{Seed128{.lo = base_.lo ^ mixed, .hi = base_.hi}};
    }

    [[nodiscard]] constexpr Seed128 base() const noexcept { return base_; }

private:
    Seed128 base_;
};

/// The whole derivation for a named object, from a world seed.
[[nodiscard]] PositionalSource positionalSourceFor(std::int64_t worldSeed,
                                                   std::string_view name) noexcept;

} // namespace stratum::rng
