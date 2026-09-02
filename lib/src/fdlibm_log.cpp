// Stratum — natural logarithm, adapted from fdlibm.
// SPDX-License-Identifier: Apache-2.0 AND SunPro
//
// ATTRIBUTION (CLAUDE.md, SPEC §2):
//   Upstream: fdlibm 5.3, file `e_log.c` (version 1.3, 95/01/18),
//             https://www.netlib.org/fdlibm/e_log.c
//   License:  SunPro / freely distributable with notice preserved — the
//             upstream notice is reproduced verbatim immediately below and
//             must not be removed. Also recorded in NOTICE.
//   Obtained from netlib. Deliberately NOT taken from OpenJDK's StrictMath,
//   which is GPL+CE and incompatible with this repository's licence.
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================
//
// Changes from upstream, all mechanical:
//   * C++ namespacing, `static_cast` in place of C casts, fixed-width types.
//   * fdlibm's __HI/__LO macros alias a double as two 32-bit words through a
//     union, which is endian-dependent and is type punning. They are
//     replaced by std::bit_cast on the 64-bit representation, which is
//     endian-neutral and well defined.
//   * `f == 0` is spelled as a bit test so the project's -Wfloat-equal stays
//     on; f is produced by `x - 1.0` with x normalised into [1, 2), so the
//     test is exactly the upstream comparison.
// The algorithm, its constants and its evaluation order are untouched:
// reassociating any expression here would change the returned bits.
//
// NOTE: this file relies on -ffp-contract=off (SPEC §5.4). Allowing the
// compiler to contract these expressions into FMAs changes the result.

#include <stratum/math/fdlibm.hpp>

#include <bit>
#include <cstdint>

namespace stratum::fdlibm {

namespace {

// Upstream constants. The hexadecimal values in the comments are the
// intended ones; the decimal forms are upstream's.
constexpr double kLn2Hi = 6.93147180369123816490e-01; // 3fe62e42 fee00000
constexpr double kLn2Lo = 1.90821492927058770002e-10; // 3dea39ef 35793c76
constexpr double kTwo54 = 1.80143985094819840000e+16; // 43500000 00000000
constexpr double kLg1 = 6.666666666666735130e-01;     // 3FE55555 55555593
constexpr double kLg2 = 3.999999999940941908e-01;     // 3FD99999 9997FA04
constexpr double kLg3 = 2.857142874366239149e-01;     // 3FD24924 94229359
constexpr double kLg4 = 2.222219843214978396e-01;     // 3FCC71C5 1D8E78AF
constexpr double kLg5 = 1.818357216161805012e-01;     // 3FC74664 96CB03DE
constexpr double kLg6 = 1.531383769920937332e-01;     // 3FC39A09 D078C69F
constexpr double kLg7 = 1.479819860511658591e-01;     // 3FC2F112 DF3E5244

/// High 32 bits of the IEEE-754 representation, as a signed word: upstream
/// tests its sign to detect negative inputs.
[[nodiscard]] std::int32_t highWord(double value) noexcept {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(value) >> 32U));
}

[[nodiscard]] std::uint32_t lowWord(double value) noexcept {
    return static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(value) & 0xFFFFFFFFU);
}

/// Upstream's `__HI(x) = word`: replaces the high 32 bits, keeping the low
/// ones. Used to normalise x into [1, 2) without touching its mantissa.
[[nodiscard]] double withHighWord(double value, std::int32_t word) noexcept {
    const std::uint64_t low = std::bit_cast<std::uint64_t>(value) & 0xFFFFFFFFU;
    const auto high = static_cast<std::uint64_t>(static_cast<std::uint32_t>(word));
    return std::bit_cast<double>((high << 32U) | low);
}

/// Exactly `value == 0.0`, including -0.0, without a float comparison.
[[nodiscard]] bool isZero(double value) noexcept {
    return (std::bit_cast<std::uint64_t>(value) & 0x7FFFFFFFFFFFFFFFULL) == 0U;
}

} // namespace

double log(double x) noexcept {
    // Upstream computes -two54/zero and (x-x)/zero to raise the divide-by-zero
    // and invalid flags while producing -inf and NaN. Kept as division by a
    // runtime zero so the IEEE results, not a folded constant, are returned.
    const volatile double zero = 0.0;

    std::int32_t hx = highWord(x);
    const std::uint32_t lx = lowWord(x);

    std::int32_t k = 0;
    if (hx < 0x00100000) { // x < 2**-1022
        if (((hx & 0x7FFFFFFF) | static_cast<std::int32_t>(lx)) == 0) {
            return -kTwo54 / zero; // log(+-0) = -inf
        }
        if (hx < 0) {
            // (x - x), not 0.0: upstream writes it this way so a NaN input
            // propagates its own payload instead of being replaced. The two
            // sides being identical is the point.
            // NOLINTNEXTLINE(misc-redundant-expression)
            return (x - x) / zero; // log(-#) = NaN
        }
        k -= 54;
        x *= kTwo54; // subnormal: scale up
        hx = highWord(x);
    }
    if (hx >= 0x7FF00000) {
        return x + x; // +inf or NaN
    }

    k += (hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    std::int32_t i = (hx + 0x95F64) & 0x100000;
    x = withHighWord(x, hx | (i ^ 0x3FF00000)); // normalise x or x/2
    k += (i >> 20);
    const double f = x - 1.0;

    double dk = 0.0;
    if ((0x000FFFFF & (2 + hx)) < 3) { // |f| < 2**-20
        if (isZero(f)) {
            if (k == 0) {
                return 0.0;
            }
            dk = static_cast<double>(k);
            return (dk * kLn2Hi) + (dk * kLn2Lo);
        }
        const double r = f * f * (0.5 - (0.33333333333333333 * f));
        if (k == 0) {
            return f - r;
        }
        dk = static_cast<double>(k);
        return (dk * kLn2Hi) - ((r - (dk * kLn2Lo)) - f);
    }

    const double s = f / (2.0 + f);
    dk = static_cast<double>(k);
    const double z = s * s;
    i = hx - 0x6147A;
    const double w = z * z;
    const std::int32_t j = 0x6B851 - hx;
    const double t1 = w * (kLg2 + (w * (kLg4 + (w * kLg6))));
    const double t2 = z * (kLg1 + (w * (kLg3 + (w * (kLg5 + (w * kLg7))))));
    i |= j;
    const double r = t2 + t1;
    if (i > 0) {
        const double hfsq = 0.5 * f * f;
        if (k == 0) {
            return f - (hfsq - (s * (hfsq + r)));
        }
        return (dk * kLn2Hi) - ((hfsq - ((s * (hfsq + r)) + (dk * kLn2Lo))) - f);
    }
    if (k == 0) {
        return f - (s * (f - r));
    }
    return (dk * kLn2Hi) - (((s * (f - r)) - (dk * kLn2Lo)) - f);
}

} // namespace stratum::fdlibm
