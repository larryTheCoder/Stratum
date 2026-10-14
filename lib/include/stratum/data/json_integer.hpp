// Stratum — a JSON integer, read at the width its field holds or not at all.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// nlohmann's `get<std::int32_t>()` narrows modulo 2^32, and
// `get<std::int64_t>()` takes an unsigned value past INT64_MAX modulo 2^64.
// Both are defined C++20, so no sanitizer reports them, and both are wrong:
// a surface rule's `{"absolute": 4294967359}` read as 63, a `sea_level` of
// 18446744073709551615 as -1, and a geometry's `min_y` of
// 18446744073709551552 as -64 — which then passed the schema's range check
// it should have failed. SPEC §5.7 lets no unmeasured wrap stand in for a
// value, and §8 asks that what cannot be read fail by name: every integer a
// pack gives is read through exactInteger, and one its field cannot hold is
// refused by the caller, which knows the field's name.

#pragma once

#include <nlohmann/json.hpp>

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>

namespace stratum::data {

/// @p value as an @p Integer exactly, or nothing: nothing for a value that is
/// not a JSON integer, and nothing for one outside @p Integer's range — never
/// a value narrowed to fit. A JSON integer is held by the parser as an int64
/// or, when it is non-negative, a uint64; past either it is a float, and not
/// an integer here.
template<std::signed_integral Integer>
[[nodiscard]] std::optional<Integer> exactInteger(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto wide = value.get<std::uint64_t>();
        if (wide > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
            return std::nullopt;
        }
        return static_cast<Integer>(wide);
    }
    if (value.is_number_integer()) {
        const auto wide = value.get<std::int64_t>();
        if (wide < std::numeric_limits<Integer>::min() ||
            wide > std::numeric_limits<Integer>::max()) {
            return std::nullopt;
        }
        return static_cast<Integer>(wide);
    }
    return std::nullopt;
}

} // namespace stratum::data
