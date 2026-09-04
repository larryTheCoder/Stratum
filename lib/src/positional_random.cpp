// Stratum — vanilla's per-position random source.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/rng/xoroshiro128.hpp>

namespace stratum::rng {

std::int64_t positionSeed(const std::int32_t x, const std::int32_t y,
                          const std::int32_t z) noexcept {
    const auto xTerm = static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(static_cast<std::uint32_t>(x) * UINT32_C(3129871))));
    std::uint64_t value =
        xTerm ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * UINT64_C(116129781)) ^
        static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    value = (value * value * UINT64_C(42317861)) + (value * UINT64_C(11));
    // arithmetic: the sign has to survive, which a logical shift loses
    return static_cast<std::int64_t>(value) >> 16U;
}

PositionalSource positionalSourceFor(const std::int64_t worldSeed,
                                     const std::string_view name) noexcept {
    const XoroshiroPositionalFactory factory{worldSeed};
    Xoroshiro128PlusPlus named = factory.fromHashOf(name);
    // Sequenced through named locals: both draws come from one generator, so
    // which is taken first is part of the answer.
    const auto lo = static_cast<std::uint64_t>(named.nextLong());
    const auto hi = static_cast<std::uint64_t>(named.nextLong());
    return PositionalSource{Seed128{.lo = lo, .hi = hi}};
}

} // namespace stratum::rng
