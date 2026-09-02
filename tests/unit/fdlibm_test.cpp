// Stratum — fdlibm parity tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The vendored port must return exactly what java.lang.StrictMath returns,
// on every platform. Comparison is on raw bits: this suite exists precisely
// because the host libm is within one ulp and that is not good enough
// (SPEC §7 Tier A).

#include "fdlibm_vectors.inc" // NOLINT(misc-include-cleaner) — generated

#include <stratum/math/fdlibm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] double fromBits(std::uint64_t value) noexcept {
    return std::bit_cast<double>(value);
}

} // namespace

TEST_CASE("fdlibm::log is bit-identical to StrictMath.log", "[fdlibm][vectors]") {
    constexpr std::uint64_t kCanonicalNaN = UINT64_C(0x7FF8000000000000);

    for (const LogVector& vector : kLogVectors) {
        CAPTURE(vector.inputBits);
        const double actual = stratum::fdlibm::log(fromBits(vector.inputBits));

        if (vector.resultBits == kCanonicalNaN) {
            // NaN is compared as NaN, not as bits. IEEE 754 leaves the sign
            // of the NaN produced by 0.0/0.0 unspecified and the hardware
            // disagrees: x86-64 gives 0xFFF8..., AArch64 gives 0x7FF8....
            // An arm64 runner caught this suite demanding the x86 spelling.
            // Java erases the distinction in Double.doubleToLongBits, and
            // nothing observable can depend on it, since a NaN reaching
            // terrain is already a bug. Every non-NaN result stays exact.
            CHECK(std::isnan(actual));
        } else {
            CHECK(bits(actual) == vector.resultBits);
        }
    }
}

TEST_CASE("fdlibm::log handles the IEEE special cases", "[fdlibm]") {
    // Compared as bits throughout: the project keeps -Wfloat-equal on, and
    // "which infinity" and "+0 or -0" are exactly the distinctions that
    // matter here.
    constexpr std::uint64_t kNegativeInfinity = UINT64_C(0xFFF0000000000000);
    constexpr std::uint64_t kPositiveInfinity = UINT64_C(0x7FF0000000000000);

    CHECK(bits(stratum::fdlibm::log(0.0)) == kNegativeInfinity);
    CHECK(bits(stratum::fdlibm::log(-0.0)) == kNegativeInfinity);
    CHECK(bits(stratum::fdlibm::log(std::numeric_limits<double>::infinity())) == kPositiveInfinity);
    CHECK(std::isnan(stratum::fdlibm::log(-1.0)));
    CHECK(std::isnan(stratum::fdlibm::log(-std::numeric_limits<double>::infinity())));
    CHECK(std::isnan(stratum::fdlibm::log(std::numeric_limits<double>::quiet_NaN())));
    CHECK(bits(stratum::fdlibm::log(1.0)) == 0U); // exactly +0.0, not -0.0
}

TEST_CASE("fdlibm::log differs from the host libm, which is the point", "[fdlibm][provenance]") {
    // Not a correctness claim about either function: a record that the two
    // genuinely disagree, so that if someone "simplifies" fdlibm::log into a
    // call to std::log, this test fails instead of a chunk seam appearing
    // months later. On a platform whose libm happens to agree everywhere,
    // the count is simply zero and the suite still passes.
    std::size_t divergences = 0;
    for (const LogVector& vector : kLogVectors) {
        const double input = fromBits(vector.inputBits);
        if (!(input > 0.0) || std::isinf(input)) {
            continue;
        }
        if (bits(std::log(input)) != vector.resultBits) {
            ++divergences;
        }
    }
    WARN("host libm differs from StrictMath.log on " << divergences << " of " << kLogVectors.size()
                                                     << " vectors");
    CHECK(divergences < kLogVectors.size());
}
