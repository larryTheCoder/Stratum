// Stratum — which spans the aquifer's keyed ranking hands to the insertion.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// detail::rankWindow keys a candidate's index into the low 32 bits of a
// 64-bit key, so a span longer than that indexes is ranked by the insertion
// itself (detail::kMaxKeyedCandidates). A size type no wider than the index
// holds no such span, and there the answer is false without a comparison
// (detail::tooLongToKey); aquifer_rank_test.cpp holds the ranking itself to
// the insertion.

#include <stratum/aquifer/selection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

using stratum::aquifer::detail::kMaxKeyedCandidates;
using stratum::aquifer::detail::tooLongToKey;

TEST_CASE("a span is too long to key only past the keys' index, whatever its size type",
          "[aquifer][selection]") {
    REQUIRE(kMaxKeyedCandidates == std::numeric_limits<std::uint32_t>::max());

    // A 32-bit size, as a 32-bit target's size_t: never too long.
    static_assert(!tooLongToKey(std::uint32_t{0}));
    static_assert(!tooLongToKey(std::uint32_t{12}));
    static_assert(!tooLongToKey(std::numeric_limits<std::uint32_t>::max()));
    static_assert(!tooLongToKey(std::uint16_t{12}));

    // A 64-bit size: too long exactly past the bound.
    static_assert(!tooLongToKey(std::uint64_t{0}));
    static_assert(!tooLongToKey(std::uint64_t{12}));
    static_assert(!tooLongToKey(std::uint64_t{kMaxKeyedCandidates}));
    static_assert(tooLongToKey(std::uint64_t{kMaxKeyedCandidates} + 1U));
    static_assert(tooLongToKey(std::numeric_limits<std::uint64_t>::max()));

    // And this target's own size_t, the one rankWindow asks with.
    CHECK_FALSE(tooLongToKey(std::size_t{12}));
    CHECK_FALSE(tooLongToKey(std::size_t{kMaxKeyedCandidates}));
}
