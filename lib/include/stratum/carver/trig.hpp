// Stratum — the carvers' table sine and cosine.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// spec/carvers-a-spec.md Q9.1-Q9.2: 65 536 single-precision entries, entry j
// the single-precision rounding of the STRICT sine of j / 10430.378350470453
// (fdlibm::sin, vendored, so every platform builds the same table). S(a)
// reads entry `trunc64(a * 10430.378350470453) & 65535`, C(a) entry
// `trunc64(a * 10430.378350470453 + 16384) & 65535`; the argument is a
// double (a float angle widened exactly first), the product and the 16384
// are double, and the truncation is toward zero, as Java's `(long)` cast:
// a floor-based index differs on every negative non-integral product. The
// table's SHA-256, over its entries as big-endian float32 in index order, is
// the spec's (tests/unit/carver_trig_test.cpp).

#pragma once

#include <array>
#include <cstddef>

namespace stratum::carver {

inline constexpr std::size_t kSinTableSize = 65536;
inline constexpr double kSinTableScale = 10430.378350470453;

/// The table, built once on first use and immutable after (thread-safe:
/// a function-local static).
[[nodiscard]] const std::array<float, kSinTableSize>& sinTable() noexcept;

/// S(a), the table sine.
[[nodiscard]] float tableSin(double a) noexcept;

/// C(a), the table cosine.
[[nodiscard]] float tableCos(double a) noexcept;

} // namespace stratum::carver
