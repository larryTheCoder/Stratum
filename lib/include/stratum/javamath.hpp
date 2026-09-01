// Stratum — Java integer and narrowing semantics.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Worldgen parity depends on reproducing Java's arithmetic exactly, and the
// places where C++ differs are the ones that silently corrupt output rather
// than crash (CLAUDE.md, "Known landmines"):
//
//   * Java's `/` and `%` overflow silently for MIN_VALUE / -1; in C++ that
//     expression is undefined behaviour.
//   * Java masks shift distances (& 31 for int, & 63 for long), so `x << 32`
//     is a no-op and `x << -1` is a shift by 31. In C++ both are undefined.
//   * Java's `>>>` has no C++ spelling; `>>` on a negative signed value is
//     an arithmetic shift.
//   * Java's narrowing casts from double saturate and map NaN to 0; the
//     equivalent C++ conversion is undefined behaviour once the value is out
//     of range.
//   * Java integer overflow wraps; C++ signed overflow is undefined.
//
// Every helper here is verified against known-answer vectors observed from a
// real JVM — see tools/vectors/JavaMathVectors.java. SPEC §5.2 makes these
// helpers mandatory: raw `%` or `>>` on a possibly-negative value is a bug
// even when the tests happen to pass.

#pragma once

#include <cassert>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace stratum::javamath {

/// The integer types whose Java counterparts (`int`, `long`) we model.
template<typename T>
concept JavaInt = std::signed_integral<T> && (sizeof(T) == 4 || sizeof(T) == 8);

namespace detail {

template<JavaInt T>
using Unsigned = std::make_unsigned_t<T>;

template<JavaInt T>
[[nodiscard]] constexpr Unsigned<T> bits(T value) noexcept {
    return static_cast<Unsigned<T>>(value);
}

/// Java's shift distances are masked to the operand width; the mask is part
/// of the language, not an implementation detail we may skip.
template<JavaInt T>
[[nodiscard]] constexpr unsigned shiftMask(int distance) noexcept {
    constexpr unsigned kWidthMask = (sizeof(T) * 8U) - 1U;
    return static_cast<unsigned>(distance) & kWidthMask;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Wrapping arithmetic — Java overflow semantics, no undefined behaviour.
// ---------------------------------------------------------------------------

template<JavaInt T>
[[nodiscard]] constexpr T wrappingAdd(T a, T b) noexcept {
    return static_cast<T>(static_cast<detail::Unsigned<T>>(detail::bits(a) + detail::bits(b)));
}

template<JavaInt T>
[[nodiscard]] constexpr T wrappingSub(T a, T b) noexcept {
    return static_cast<T>(static_cast<detail::Unsigned<T>>(detail::bits(a) - detail::bits(b)));
}

template<JavaInt T>
[[nodiscard]] constexpr T wrappingMul(T a, T b) noexcept {
    return static_cast<T>(static_cast<detail::Unsigned<T>>(detail::bits(a) * detail::bits(b)));
}

template<JavaInt T>
[[nodiscard]] constexpr T wrappingNegate(T value) noexcept {
    return static_cast<T>(static_cast<detail::Unsigned<T>>(0U - detail::bits(value)));
}

// ---------------------------------------------------------------------------
// Division — Math.floorDiv / Math.floorMod, and Java's truncating / and %.
//
// Precondition: y != 0. Java throws ArithmeticException; worldgen never
// divides by zero, so this is an assertion rather than a runtime branch in
// the hot path.
// ---------------------------------------------------------------------------

/// Java's `x / y`: truncates toward zero, and wraps for MIN_VALUE / -1.
template<JavaInt T>
[[nodiscard]] constexpr T truncDiv(T x, T y) noexcept {
    assert(y != 0);
    if (y == -1) {
        return wrappingNegate(x);
    }
    return static_cast<T>(x / y);
}

/// Java's `x % y`: sign follows the dividend, and is 0 for MIN_VALUE % -1.
template<JavaInt T>
[[nodiscard]] constexpr T remainder(T x, T y) noexcept {
    assert(y != 0);
    if (y == -1) {
        return 0;
    }
    return static_cast<T>(x % y);
}

/// `Math.floorDiv`: rounds toward negative infinity.
template<JavaInt T>
[[nodiscard]] constexpr T floorDiv(T x, T y) noexcept {
    assert(y != 0);
    if (y == -1) {
        // floor(x / -1) is exactly -x, which wraps for MIN_VALUE just as
        // Math.floorDiv does.
        return wrappingNegate(x);
    }
    const T quotient = static_cast<T>(x / y);
    const T rem = static_cast<T>(x % y);
    // Truncation rounded toward zero; step down when the exact quotient was
    // negative and inexact.
    if (rem != 0 && ((x < 0) != (y < 0))) {
        return static_cast<T>(quotient - 1);
    }
    return quotient;
}

/// `Math.floorMod`: sign follows the divisor, so the result of
/// floorMod(x, n) for positive n is always in [0, n).
template<JavaInt T>
[[nodiscard]] constexpr T floorMod(T x, T y) noexcept {
    assert(y != 0);
    return wrappingSub(x, wrappingMul(floorDiv(x, y), y));
}

// ---------------------------------------------------------------------------
// Shifts — Java's masked distances; `ushr` is Java's `>>>`.
// ---------------------------------------------------------------------------

template<JavaInt T>
[[nodiscard]] constexpr T shl(T value, int distance) noexcept {
    return static_cast<T>(
        static_cast<detail::Unsigned<T>>(detail::bits(value) << detail::shiftMask<T>(distance)));
}

/// Arithmetic right shift: Java's `>>`, sign-extending.
template<JavaInt T>
[[nodiscard]] constexpr T shr(T value, int distance) noexcept {
    return static_cast<T>(value >> detail::shiftMask<T>(distance));
}

/// Logical right shift: Java's `>>>`, shifting zeroes in.
template<JavaInt T>
[[nodiscard]] constexpr T ushr(T value, int distance) noexcept {
    return static_cast<T>(
        static_cast<detail::Unsigned<T>>(detail::bits(value) >> detail::shiftMask<T>(distance)));
}

// ---------------------------------------------------------------------------
// Narrowing conversions.
// ---------------------------------------------------------------------------

/// Java's `(int)` cast applied to a long: keeps the low 32 bits.
[[nodiscard]] constexpr std::int32_t toInt(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}

/// Java's `(int)` cast applied to a double: NaN becomes 0, out-of-range
/// values saturate, everything else truncates toward zero.
[[nodiscard]] inline std::int32_t doubleToInt(double value) noexcept {
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    if (std::isnan(value)) {
        return 0;
    }
    if (value <= static_cast<double>(kMin)) {
        return kMin;
    }
    if (value >= static_cast<double>(kMax)) {
        return kMax;
    }
    return static_cast<std::int32_t>(value);
}

/// Java's `(long)` cast applied to a double: same rules, 64-bit range.
[[nodiscard]] inline std::int64_t doubleToLong(double value) noexcept {
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    if (std::isnan(value)) {
        return 0;
    }
    // 2^63 is exactly representable; 2^63 - 1 is not, so both bounds are
    // compared against the exactly representable powers of two.
    if (value <= static_cast<double>(kMin)) {
        return kMin;
    }
    if (value >= -static_cast<double>(kMin)) {
        return kMax;
    }
    return static_cast<std::int64_t>(value);
}

/// `(int) Math.floor(value)` — the shape most worldgen coordinate maths
/// needs, with Java's saturation preserved.
[[nodiscard]] inline std::int32_t floorToInt(double value) noexcept {
    return doubleToInt(std::floor(value));
}

/// `(long) Math.floor(value)`.
[[nodiscard]] inline std::int64_t floorToLong(double value) noexcept {
    return doubleToLong(std::floor(value));
}

} // namespace stratum::javamath
