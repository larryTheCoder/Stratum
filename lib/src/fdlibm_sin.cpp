// Stratum — sine and cosine, adapted from fdlibm.
// SPDX-License-Identifier: Apache-2.0 AND SunPro
//
// ATTRIBUTION (CLAUDE.md, SPEC §2):
//   Upstream: fdlibm 5.3, files `s_sin.c` (1.3, 95/01/18), `s_cos.c`
//             (1.3, 95/01/18), `k_sin.c` (1.3, 95/01/18), `k_cos.c`
//             (1.3, 95/01/18) and `e_rem_pio2.c` (1.4, 95/01/18),
//             https://netlib.org/fdlibm/
//   License:  SunPro / freely distributable with notice preserved — the
//             upstream notice is reproduced verbatim immediately below and
//             must not be removed. Also recorded in NOTICE.
//   Obtained from netlib, as fdlibm_log.cpp was. Deliberately NOT taken from
//   OpenJDK's StrictMath, which is GPL+CE and incompatible with this
//   repository's licence.
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
// WHY. The carvers' sine table (spec/carvers-a-spec.md Q9.1-Q9.2) is the
// single-precision rounding of the STRICT sine at 65 536 points, and a host
// libm is not bit-identical across platforms (SPEC §5). The ore feature's
// direction (feature/feature.hpp) reads the sine and the cosine both.
//
// Changes from upstream, all mechanical:
//   * C++ namespacing, `static_cast` in place of C casts, fixed-width types.
//   * __HI/__LO are replaced by std::bit_cast on the 64-bit representation,
//     which is endian-neutral and well defined; shifts of the high word are
//     made on its unsigned form.
//   * ONLY the small and medium argument paths of `__ieee754_rem_pio2` are
//     carried: |x| up to 2^19 * pi/2 (high word 0x413921fb). The large path
//     needs `__kernel_rem_pio2` and its 2/pi table, which no caller needs:
//     the sine table's arguments lie in [0, 2 pi). A larger argument is a
//     precondition violation, asserted, and answered with NaN, never with a
//     value computed some other way.
// The algorithm, its constants and its evaluation order are untouched:
// reassociating any expression here would change the returned bits.
//
// NOTE: this file relies on -ffp-contract=off (SPEC §5.4).

#include <stratum/math/fdlibm.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

namespace stratum::fdlibm {

namespace {

[[nodiscard]] std::int32_t highWord(const double x) noexcept {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(x) >> 32U));
}

[[nodiscard]] double fromWords(const std::uint32_t high, const std::uint32_t low) noexcept {
    return std::bit_cast<double>((static_cast<std::uint64_t>(high) << 32U) | low);
}

// k_sin.c
constexpr double kHalf = 5.00000000000000000000e-01;
constexpr double kS1 = -1.66666666666666324348e-01;
constexpr double kS2 = 8.33333333332248946124e-03;
constexpr double kS3 = -1.98412698298579493134e-04;
constexpr double kS4 = 2.75573137070700676789e-06;
constexpr double kS5 = -2.50507602534068634195e-08;
constexpr double kS6 = 1.58969099521155010221e-10;

[[nodiscard]] double kernelSin(const double x, const double y, const int iy) noexcept {
    const std::int32_t ix = highWord(x) & 0x7fffffff;
    if (ix < 0x3e400000) {              // |x| < 2**-27
        if (static_cast<int>(x) == 0) { // generate inexact
            return x;
        }
    }
    const double z = x * x;
    const double v = z * x;
    const double r = kS2 + z * (kS3 + z * (kS4 + z * (kS5 + z * kS6)));
    if (iy == 0) {
        return x + v * (kS1 + z * r);
    }
    return x - ((z * (kHalf * y - v * r) - y) - v * kS1);
}

// k_cos.c
constexpr double kOne = 1.00000000000000000000e+00;
constexpr double kC1 = 4.16666666666666019037e-02;
constexpr double kC2 = -1.38888888888741095749e-03;
constexpr double kC3 = 2.48015872894767294178e-05;
constexpr double kC4 = -2.75573143513906633035e-07;
constexpr double kC5 = 2.08757232129817482790e-09;
constexpr double kC6 = -1.13596475577881948265e-11;

[[nodiscard]] double kernelCos(const double x, const double y) noexcept {
    const std::int32_t ix = highWord(x) & 0x7fffffff;
    if (ix < 0x3e400000) {              // if x < 2**27
        if (static_cast<int>(x) == 0) { // generate inexact
            return kOne;
        }
    }
    const double z = x * x;
    const double r = z * (kC1 + z * (kC2 + z * (kC3 + z * (kC4 + z * (kC5 + z * kC6)))));
    if (ix < 0x3FD33333) { // if |x| < 0.3
        return kOne - (0.5 * z - (z * r - x * y));
    }
    double qx = 0.0;
    if (ix > 0x3fe90000) { // x > 0.78125
        qx = 0.28125;
    } else {
        qx = fromWords(static_cast<std::uint32_t>(ix - 0x00200000), 0U); // x/4
    }
    const double hz = 0.5 * z - qx;
    const double a = kOne - qx;
    return a - (hz - (z * r - x * y));
}

// e_rem_pio2.c, small and medium paths.
constexpr std::array<std::int32_t, 32> kNpio2Hw = {
    0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB, 0x402921FB,
    0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB,
    0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
    0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB,
};
constexpr double kInvPio2 = 6.36619772367581382433e-01;
constexpr double kPio2One = 1.57079632673412561417e+00;
constexpr double kPio2OneTail = 6.07710050650619224932e-11;
constexpr double kPio2Two = 6.07710050630396597660e-11;
constexpr double kPio2TwoTail = 2.02226624879595063154e-21;
constexpr double kPio2Three = 2.02226624871116645580e-21;
constexpr double kPio2ThreeTail = 8.47842766036889956997e-32;

/// PRECONDITION: |x| <= 2^19 * pi/2 (high word <= 0x413921fb), finite.
[[nodiscard]] int remPio2(const double x, std::array<double, 2>& y) noexcept {
    const std::int32_t hx = highWord(x);
    const std::int32_t ix = hx & 0x7fffffff;
    if (ix <= 0x3fe921fb) { // |x| ~<= pi/4 , no need for reduction
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002d97c) { // |x| < 3pi/4, special case with n=+-1
        if (hx > 0) {
            double z = x - kPio2One;
            if (ix != 0x3ff921fb) { // 33+53 bit pi is good enough
                y[0] = z - kPio2OneTail;
                y[1] = (z - y[0]) - kPio2OneTail;
            } else { // near pi/2, use 33+33+53 bit pi
                z -= kPio2Two;
                y[0] = z - kPio2TwoTail;
                y[1] = (z - y[0]) - kPio2TwoTail;
            }
            return 1;
        }
        // negative x
        double z = x + kPio2One;
        if (ix != 0x3ff921fb) { // 33+53 bit pi is good enough
            y[0] = z + kPio2OneTail;
            y[1] = (z - y[0]) + kPio2OneTail;
        } else { // near pi/2, use 33+33+53 bit pi
            z += kPio2Two;
            y[0] = z + kPio2TwoTail;
            y[1] = (z - y[0]) + kPio2TwoTail;
        }
        return -1;
    }
    // |x| ~<= 2^19*(pi/2), medium size
    assert(ix <= 0x413921fb);
    double t = std::fabs(x);
    const int n = static_cast<int>(t * kInvPio2 + kHalf);
    const auto fn = static_cast<double>(n);
    double r = t - fn * kPio2One;
    double w = fn * kPio2OneTail; // 1st round good to 85 bit
    if (n < 32 && ix != kNpio2Hw[static_cast<std::size_t>(n - 1)]) {
        y[0] = r - w; // quick check no cancellation
    } else {
        const std::int32_t j = ix >> 20;
        y[0] = r - w;
        std::int32_t i = j - static_cast<std::int32_t>(
                                 (static_cast<std::uint32_t>(highWord(y[0])) >> 20U) & 0x7ffU);
        if (i > 16) { // 2nd iteration needed, good to 118
            t = r;
            w = fn * kPio2Two;
            r = t - w;
            w = fn * kPio2TwoTail - ((t - r) - w);
            y[0] = r - w;
            i = j - static_cast<std::int32_t>((static_cast<std::uint32_t>(highWord(y[0])) >> 20U) &
                                              0x7ffU);
            if (i > 49) { // 3rd iteration need, 151 bits acc
                t = r;    // will cover all possible cases
                w = fn * kPio2Three;
                r = t - w;
                w = fn * kPio2ThreeTail - ((t - r) - w);
                y[0] = r - w;
            }
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

} // namespace

double sin(const double x) noexcept {
    const double z = 0.0;
    // High word of x.
    const std::int32_t ix = highWord(x) & 0x7fffffff;

    // |x| ~< pi/4
    if (ix <= 0x3fe921fb) {
        return kernelSin(x, z, 0);
    }
    // sin(Inf or NaN) is NaN
    if (ix >= 0x7ff00000) {
        return x - x;
    }
    // Past the medium path: not carried (the header says why).
    if (ix > 0x413921fb) {
        assert(false && "fdlibm::sin: |x| past 2^19 * pi/2 is not carried");
        return std::numeric_limits<double>::quiet_NaN();
    }
    // argument reduction needed
    std::array<double, 2> y{};
    const int n = remPio2(x, y);
    switch (n & 3) {
        case 0:
            return kernelSin(y[0], y[1], 1);
        case 1:
            return kernelCos(y[0], y[1]);
        case 2:
            return -kernelSin(y[0], y[1], 1);
        default:
            return -kernelCos(y[0], y[1]);
    }
}

double cos(const double x) noexcept {
    const double z = 0.0;
    // High word of x.
    const std::int32_t ix = highWord(x) & 0x7fffffff;

    // |x| ~< pi/4
    if (ix <= 0x3fe921fb) {
        return kernelCos(x, z);
    }
    // cos(Inf or NaN) is NaN
    if (ix >= 0x7ff00000) {
        return x - x;
    }
    // Past the medium path: not carried (the header says why).
    if (ix > 0x413921fb) {
        assert(false && "fdlibm::cos: |x| past 2^19 * pi/2 is not carried");
        return std::numeric_limits<double>::quiet_NaN();
    }
    // argument reduction needed
    std::array<double, 2> y{};
    const int n = remPio2(x, y);
    switch (n & 3) {
        case 0:
            return kernelCos(y[0], y[1]);
        case 1:
            return -kernelSin(y[0], y[1], 1);
        case 2:
            return -kernelCos(y[0], y[1]);
        default:
            return kernelSin(y[0], y[1], 1);
    }
}

} // namespace stratum::fdlibm
