// Stratum — the Perlin sampler's body, inline for the octave loops.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ADAPTED (CLAUDE.md, NOTICE): from cubiomes (https://github.com/Cubitect/
// cubiomes, MIT License, Copyright (c) 2020 Cubitect), noise.c at e61f905 —
// `samplePerlin` and the gradient tables of its disabled `indexedLerp`, the
// adaptation perlin.cpp's header records function by function. Nothing here
// is new: this is PerlinNoise::sample's body, moved where the loops that sum
// many octaves (OctaveNoise, NormalNoise, BlendedNoise) can inline it rather
// than call it once per octave.
//
// INTERNAL. Included by perlin.cpp and blended.cpp; nothing outside
// lib/src/ should need it, and the public way in stays PerlinNoise::sample.
//
// EVERY DOUBLE HERE IS THE ONE PerlinNoise::sample ALWAYS COMPUTED: the same
// operations on the same operands in the same order, with the gradient dot,
// the fade and the lerps spelled exactly as before. What changed is how
// `floor` is reached on x86-64 builds without SSE4.1, where the compiler has
// no rounding instruction and std::floor costs a range test, a convert pair
// and a sign copy; see latticeAxis() and floorExceptNegativeZero() for why
// each replacement is the same double. tests/unit/perlin_kernel_test.cpp
// holds a verbatim copy of the previous implementation and compares the two
// by their bytes. This file depends on -ffp-contract=off (SPEC §5.4) like
// perlin.cpp does.

#pragma once

#include <stratum/noise/perlin.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

/// Inlined even where the compiler's size heuristics would decline: the
/// kernel is some two hundred instructions, past GCC's -O2 limit for an
/// inline function, and the point of this header is that the octave loops
/// take it without a call. Changes where the code sits, never what it
/// computes. MSVC gets plain `inline`: a __forceinline it declines is
/// warning C4714 at /W4, which the CI's /WX makes an error, and it is free
/// to decline one this size.
#if defined(__GNUC__) || defined(__clang__)
#define STRATUM_NOISE_INLINE [[gnu::always_inline]] inline
#else
#define STRATUM_NOISE_INLINE inline
#endif

namespace stratum::noise::detail {

/// The 16 gradient directions of improved Perlin noise. Twelve distinct
/// edges of a cube, with four repeated to make the index a cheap mask.
inline constexpr std::array<double, 16> kGradientX = {1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0,  -1.0,
                                                      0.0, 0.0,  0.0, 0.0,  1.0, 0.0,  -1.0, 0.0};
inline constexpr std::array<double, 16> kGradientY = {1.0, 1.0,  -1.0, -1.0, 0.0, 0.0,  0.0, 0.0,
                                                      1.0, -1.0, 1.0,  -1.0, 1.0, -1.0, 1.0, -1.0};
inline constexpr std::array<double, 16> kGradientZ = {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, -1.0, -1.0,
                                                      1.0, 1.0, -1.0, -1.0, 0.0, 1.0, 0.0,  -1.0};

[[nodiscard]] STRATUM_NOISE_INLINE double gradientDot(std::uint32_t index, double x, double y,
                                                      double z) noexcept {
    const std::size_t slot = index & 0x0FU;
    return (kGradientX[slot] * x) + (kGradientY[slot] * y) + (kGradientZ[slot] * z);
}

/// Perlin's quintic fade, 6t^5 - 15t^4 + 10t^3, written the way it is
/// evaluated rather than the way it is written down: the nesting is part of
/// the result.
[[nodiscard]] STRATUM_NOISE_INLINE double fade(double t) noexcept {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

[[nodiscard]] STRATUM_NOISE_INLINE double lerp(double t, double a, double b) noexcept {
    return a + (t * (b - a));
}

/// Exactly `value == 0.0`, including -0.0, without a float comparison. These
/// are genuine exact-zero branches in the algorithm — "this octave is
/// absent", "y is not folded" — not tolerance tests, and the project keeps
/// -Wfloat-equal on.
[[nodiscard]] inline bool isZero(double value) noexcept {
    return (std::bit_cast<std::uint64_t>(value) & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0U;
}

/// Whether this build reaches floor through the integer conversions below
/// rather than through std::floor. Only x86-64 without SSE4.1 does: there
/// std::floor is a range test, two conversions and a sign copy, while on
/// ARM64 (`frintm`), on SSE4.1 (`roundsd`) and on 32-bit x86 (where a 64-bit
/// conversion is itself a library call) std::floor is already the cheap
/// form, and the code there is exactly what it was.
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__SSE4_1__)
inline constexpr bool kFloorByConversion = true;
#else
inline constexpr bool kFloorByConversion = false;
#endif

/// `std::floor(value)`, bit for bit, for every double but -0.0 — whose floor
/// is -0.0 and which this returns as +0.0 on the conversion path. Both
/// callers rule it out by construction: each passes a sum whose right operand
/// is a positive constant (`q + 0.5`, `q + 1e-7`), and IEEE addition gives
/// -0.0 only when both operands are -0.0. Asserted in debug builds.
///
/// The conversion path: every double of magnitude 2^52 or more is an integer
/// already and is its own floor, and so are the infinities; NaN fails the
/// magnitude test and comes back as itself, as std::floor returns it. Below
/// 2^52 the truncation is exact in int64, converts back exactly, and is the
/// floor unless it rounded a negative non-integer up, which the comparison
/// catches and one subtraction corrects — exactly, as every integer below
/// 2^53 is a double. A result of zero is +0.0, which is std::floor's for
/// every input in (-1, 1) but -0.0.
[[nodiscard]] STRATUM_NOISE_INLINE double floorExceptNegativeZero(double value) noexcept {
    assert(std::bit_cast<std::uint64_t>(value) != UINT64_C(0x8000000000000000));
    if constexpr (kFloorByConversion) {
        if (!(std::fabs(value) < 4503599627370496.0)) {
            return value;
        }
        const auto truncated = static_cast<double>(static_cast<std::int64_t>(value));
        return value < truncated ? truncated - 1.0 : truncated;
    } else {
        return std::floor(value);
    }
}

/// One axis of the lattice: the offset into the cell, `shifted -
/// floor(shifted)`, and the cell's low byte, `(uint8)(int32)floor(shifted)`
/// — the two numbers PerlinNoise::sample derives from the floor.
struct LatticeAxis {
    double local;
    std::uint32_t cell;
};

/// PRECONDITION (PerlinNoise::sample's): floor(@p shifted) in int32, NaN
/// excluded. Asserted in debug builds.
///
/// The conversion path is the same pair of numbers. Under the precondition
/// the truncation is exact in int64, and less one exactly when it rounded a
/// negative non-integer up, so `floored` IS floor(shifted) and its low byte
/// is the int32's. The offset is then `shifted + (double)(-floored)`, which
/// is `shifted - floor(shifted)` to the bit: IEEE defines a - b as a + (-b),
/// the negation is exact at this range, and the one place the two spellings
/// could differ is the sign of a zero — for `floored` = 0 the converted -0
/// is +0, so -0.0 + (+0.0) gives the +0.0 that -0.0 - floor(-0.0) =
/// -0.0 - (-0.0) gives. (`shifted - (double)floored` would not: it is -0.0
/// there.) The offset is never -0.0 either way.
[[nodiscard]] STRATUM_NOISE_INLINE LatticeAxis latticeAxis(double shifted) noexcept {
    assert(shifted >= -2147483648.0 && shifted < 2147483648.0);
    if constexpr (kFloorByConversion) {
        const auto truncated = static_cast<std::int64_t>(shifted);
        const std::int64_t floored =
            truncated - static_cast<std::int64_t>(shifted < static_cast<double>(truncated));
        return {.local = shifted + static_cast<double>(-floored),
                .cell = static_cast<std::uint32_t>(floored) & 0xFFU};
    } else {
        const double cell = std::floor(shifted);
        return {.local = shifted - cell,
                .cell = static_cast<std::uint32_t>(static_cast<std::int32_t>(cell)) & 0xFFU};
    }
}

/// The y fold, `localY -= floor((min(yMax, localY) / yScale) + 1e-7) *
/// yScale`, where @p Folds; @p localY itself where not. The epsilon and why
/// it is load-bearing: perlin.cpp, at PerlinNoise::sample.
template<bool Folds>
[[nodiscard]] STRATUM_NOISE_INLINE double foldY(double localY, double yScale,
                                                double yMax) noexcept {
    if constexpr (Folds) {
        const double clamped = (yMax < localY) ? yMax : localY;
        return localY - (floorExceptNegativeZero((clamped / yScale) + 1.0e-7) * yScale);
    } else {
        static_cast<void>(yScale);
        static_cast<void>(yMax);
        return localY;
    }
}

/// PerlinNoise::sample, inline. @p Folds false is the five-argument form at
/// a yScale of zero (and so the three-argument form), which skips the fold
/// exactly where that form's isZero(yScale) test does; @p yScale and @p yMax
/// are not read then.
///
/// PRECONDITION: PerlinNoise::sample's — every axis's floor(coordinate +
/// origin) in int32. Asserted in debug builds.
template<bool Folds>
[[nodiscard]] STRATUM_NOISE_INLINE double samplePerlin(const PerlinNoise& noise, double x, double y,
                                                       double z, double yScale,
                                                       double yMax) noexcept {
    const std::array<std::uint8_t, 256>& permutation = noise.permutation();
    const auto at = [&permutation](std::uint32_t index) noexcept -> std::uint32_t {
        return permutation[index & 0xFFU];
    };

    const LatticeAxis axisX = latticeAxis(x + noise.originX());
    const LatticeAxis axisY = latticeAxis(y + noise.originY());
    const LatticeAxis axisZ = latticeAxis(z + noise.originZ());

    const double localX = axisX.local;
    const double localZ = axisZ.local;

    const double fadeX = fade(localX);
    const double fadeY = fade(axisY.local);
    const double fadeZ = fade(localZ);

    // The fade above keeps the unfolded offset; the gradients below see the
    // folded one.
    const double localY = foldY<Folds>(axisY.local, yScale, yMax);

    // Each corner hash is the byte the previous code's uint8 arithmetic
    // kept: a permutation entry plus a lattice byte, reduced mod 256.
    const std::uint32_t cornerA = (at(axisX.cell) + axisY.cell) & 0xFFU;
    const std::uint32_t cornerB = (at(axisX.cell + 1U) + axisY.cell) & 0xFFU;

    const std::uint32_t cornerAA = (at(cornerA) + axisZ.cell) & 0xFFU;
    const std::uint32_t cornerBA = (at(cornerB) + axisZ.cell) & 0xFFU;
    const std::uint32_t cornerAB = (at(cornerA + 1U) + axisZ.cell) & 0xFFU;
    const std::uint32_t cornerBB = (at(cornerB + 1U) + axisZ.cell) & 0xFFU;

    const double x0y0z0 = gradientDot(at(cornerAA), localX, localY, localZ);
    const double x1y0z0 = gradientDot(at(cornerBA), localX - 1.0, localY, localZ);
    const double x0y1z0 = gradientDot(at(cornerAB), localX, localY - 1.0, localZ);
    const double x1y1z0 = gradientDot(at(cornerBB), localX - 1.0, localY - 1.0, localZ);
    const double x0y0z1 = gradientDot(at(cornerAA + 1U), localX, localY, localZ - 1.0);
    const double x1y0z1 = gradientDot(at(cornerBA + 1U), localX - 1.0, localY, localZ - 1.0);
    const double x0y1z1 = gradientDot(at(cornerAB + 1U), localX, localY - 1.0, localZ - 1.0);
    const double x1y1z1 = gradientDot(at(cornerBB + 1U), localX - 1.0, localY - 1.0, localZ - 1.0);

    const double y0z0 = lerp(fadeX, x0y0z0, x1y0z0);
    const double y1z0 = lerp(fadeX, x0y1z0, x1y1z0);
    const double y0z1 = lerp(fadeX, x0y0z1, x1y0z1);
    const double y1z1 = lerp(fadeX, x0y1z1, x1y1z1);

    return lerp(fadeZ, lerp(fadeY, y0z0, y1z0), lerp(fadeY, y0z1, y1z1));
}

/// The five-argument PerlinNoise::sample: the fold only where yScale is not
/// zero, as that form decides it. The folded body is inline; the unfolded
/// one is the out-of-line three-argument sample, the same double, because
/// a second inline copy beside the first is merged with it by the compiler
/// into one body that runs out of registers — measured, as spills on the
/// folded path that BlendedNoise takes for every octave.
[[nodiscard]] STRATUM_NOISE_INLINE double samplePerlinFoldable(const PerlinNoise& noise, double x,
                                                               double y, double z, double yScale,
                                                               double yMax) noexcept {
    if (isZero(yScale)) {
        return noise.sample(x, y, z);
    }
    return samplePerlin<true>(noise, x, y, z, yScale, yMax);
}

} // namespace stratum::noise::detail
