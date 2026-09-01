// Stratum — fdlibm-exact transcendental functions.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Java's StrictMath is fdlibm, so it returns the same bits on every JVM and
// every platform. A host libm does not: glibc's std::log differs from
// fdlibm by one ulp on some inputs, which is a Tier-A parity failure
// (SPEC §7) wherever vanilla worldgen reaches a transcendental.
//
// These functions are therefore the only sanctioned spelling in
// parity-critical code. Reaching for <cmath> instead is a bug even when the
// tests happen to pass, in the same way that raw `%` on a negative value is
// a bug — with one exception: std::sqrt is IEEE-754 correctly rounded and
// therefore already identical to StrictMath.sqrt.
//
// Verified bit-for-bit against StrictMath known-answer vectors observed from
// a real JVM (tools/vectors/StrictMathVectors.java).

#pragma once

namespace stratum::fdlibm {

/// Natural logarithm, bit-identical to java.lang.StrictMath.log.
///
/// Special cases follow IEEE 754 and Java: log(+-0) is -inf, log(x < 0) is
/// NaN, log(+inf) is +inf, and log(NaN) is that NaN.
[[nodiscard]] double log(double x) noexcept;

} // namespace stratum::fdlibm
