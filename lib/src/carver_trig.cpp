// Stratum — the carvers' table sine and cosine (carver/trig.hpp).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/carver/trig.hpp>
#include <stratum/javamath.hpp>
#include <stratum/math/fdlibm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace stratum::carver {

namespace {

[[nodiscard]] std::array<float, kSinTableSize> buildTable() noexcept {
    std::array<float, kSinTableSize> table{};
    for (std::size_t j = 0; j < kSinTableSize; ++j) {
        table[j] = static_cast<float>(fdlibm::sin(static_cast<double>(j) / kSinTableScale));
    }
    return table;
}

[[nodiscard]] std::size_t indexOf(const double scaled) noexcept {
    // Java's `(long)` cast — toward zero, saturating, NaN to 0 — then the
    // low 16 bits, as `& 65535` on the 64-bit value takes them.
    const auto wide = static_cast<std::uint64_t>(javamath::doubleToLong(scaled));
    return static_cast<std::size_t>(wide & 0xFFFFU);
}

} // namespace

const std::array<float, kSinTableSize>& sinTable() noexcept {
    static const std::array<float, kSinTableSize> kTable = buildTable();
    return kTable;
}

float tableSin(const double a) noexcept {
    return sinTable()[indexOf(a * kSinTableScale)];
}

float tableCos(const double a) noexcept {
    return sinTable()[indexOf((a * kSinTableScale) + 16384.0)];
}

} // namespace stratum::carver
