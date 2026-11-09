// Stratum — fdlibm's sine and the carvers' sine table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// fdlibm::sin against java.lang.StrictMath.sin (fdlibm_vectors.inc, generated
// by tools/vectors/StrictMathVectors.java), and the table it builds against
// the digest spec/carvers-a-spec.md Q9.2 records.

#include "fdlibm_vectors.inc"
#include "support/sha256.hpp"

#include <stratum/carver/trig.hpp>
#include <stratum/math/fdlibm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_CASE("fdlibm::sin is bit-identical to StrictMath.sin", "[fdlibm][vectors]") {
    std::size_t checked = 0;
    for (const SinVector& vector : kSinVectors) {
        const auto input = std::bit_cast<double>(vector.inputBits);
        const double result = stratum::fdlibm::sin(input);
        INFO("sin(" << input << ")");
        if (std::isnan(result)) {
            CHECK(vector.resultBits == UINT64_C(0x7FF8000000000000));
        } else {
            CHECK(std::bit_cast<std::uint64_t>(result) == vector.resultBits);
        }
        ++checked;
    }
    CHECK(checked > 1000U);
}

TEST_CASE("the carvers' sine table is the spec's, to its SHA-256", "[carver][trig]") {
    // spec/carvers-a-spec.md Q9.2: over the entries as big-endian IEEE-754
    // float32 in index order.
    const auto& table = stratum::carver::sinTable();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(table.size() * 4U);
    for (const float entry : table) {
        const auto word = std::bit_cast<std::uint32_t>(entry);
        bytes.push_back(static_cast<std::uint8_t>(word >> 24U));
        bytes.push_back(static_cast<std::uint8_t>(word >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(word >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(word));
    }
    CHECK(stratum::test::sha256Hex(bytes) ==
          "87a67a39fff59636acb04edcda4ef622831037291f11fb61586c57596771dac2");
}

TEST_CASE("the table is read by truncation toward zero, the cosine a quarter on",
          "[carver][trig]") {
    using stratum::carver::kSinTableScale;
    using stratum::carver::sinTable;
    using stratum::carver::tableCos;
    using stratum::carver::tableSin;
    const auto& table = sinTable();
    const auto bits = [](float v) { return std::bit_cast<std::uint32_t>(v); };
    // Positive: index floor = trunc.
    CHECK(bits(tableSin(1.0)) == bits(table[static_cast<std::size_t>(1.0 * kSinTableScale)]));
    // Negative and not integral: trunc(-1.5 index) is -1, so entry 65535,
    // where a floor would read 65534 (spec Q9.1).
    const double negative = -1.5 / kSinTableScale;
    CHECK(bits(tableSin(negative)) == bits(table[65535]));
    CHECK(bits(tableSin(negative)) != bits(table[65534]));
    // The cosine adds 16384 before truncating.
    CHECK(bits(tableCos(0.0)) == bits(table[16384]));
    CHECK(bits(tableCos(1.0)) ==
          bits(table[static_cast<std::size_t>((1.0 * kSinTableScale) + 16384.0) & 0xFFFFU]));
    // Wrapping past the table's end.
    CHECK(bits(tableSin(65536.0 / kSinTableScale)) == bits(table[0]));
}
