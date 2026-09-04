// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/lattice.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <cmath>

namespace stratum::aquifer {

std::int32_t spreadOffset(const double spread) noexcept {
    // Two floors, both toward negative infinity: std::floor for the scaling,
    // then floorDiv for the grouping into threes. Writing the second as `/ 3`
    // would truncate toward zero and put every negative spread one step high.
    const auto scaled = static_cast<std::int32_t>(std::floor(spread * 10.0));
    return 3 * javamath::floorDiv(scaled, 3);
}

std::int32_t fluidLevel(const std::int32_t base, const double spread) noexcept {
    return base + spreadOffset(spread);
}

CellIndex cellOf(const std::int32_t x, const std::int32_t y, const std::int32_t z) noexcept {
    return CellIndex{.x = javamath::floorDiv(x, kCellPitchX),
                     .y = javamath::floorDiv(y, kCellPitchY),
                     .z = javamath::floorDiv(z, kCellPitchZ)};
}

std::int32_t baseLevel(const std::int32_t y, const std::int32_t preliminarySurface) noexcept {
    const std::int32_t onLattice = (kBasePitch * javamath::floorDiv(y, kBasePitch)) + kBasePhase;
    return std::min(onLattice, preliminarySurface);
}

std::int64_t positionSeed(const std::int32_t cx, const std::int32_t cy,
                          const std::int32_t cz) noexcept {
    const auto xTerm = static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(static_cast<std::uint32_t>(cx) * UINT32_C(3129871))));
    std::uint64_t value =
        xTerm ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(cz)) * UINT64_C(116129781)) ^
        static_cast<std::uint64_t>(static_cast<std::int64_t>(cy));
    value = (value * value * UINT64_C(42317861)) + (value * UINT64_C(11));
    // arithmetic shift: the sign has to survive, which a logical shift loses
    return static_cast<std::int64_t>(value) >> 16U;
}

CentreSource::CentreSource(const std::int64_t worldSeed) noexcept {
    const rng::XoroshiroPositionalFactory factory{worldSeed};
    rng::Xoroshiro128PlusPlus named = factory.fromHashOf("minecraft:aquifer");
    // Sequenced through named locals: both draws come from one generator, so
    // which is taken first is part of the answer.
    const auto lo = static_cast<std::uint64_t>(named.nextLong());
    const auto hi = static_cast<std::uint64_t>(named.nextLong());
    base_ = rng::Seed128{.lo = lo, .hi = hi};
}

Jitter CentreSource::jitterOf(const std::int32_t cx, const std::int32_t cy,
                              const std::int32_t cz) const noexcept {
    const auto mixed = static_cast<std::uint64_t>(positionSeed(cx, cy, cz));
    rng::Xoroshiro128PlusPlus source{rng::Seed128{.lo = base_.lo ^ mixed, .hi = base_.hi}};
    const std::int32_t jx = source.nextInt(kJitterBoundX);
    const std::int32_t jy = source.nextInt(kJitterBoundY);
    const std::int32_t jz = source.nextInt(kJitterBoundZ);
    return Jitter{.x = jx, .y = jy, .z = jz};
}

CellIndex CentreSource::centreOf(const std::int32_t cx, const std::int32_t cy,
                                 const std::int32_t cz) const noexcept {
    const Jitter jitter = jitterOf(cx, cy, cz);
    return CellIndex{.x = (cx * kCellPitchX) + jitter.x,
                     .y = (cy * kCellPitchY) + jitter.y,
                     .z = (cz * kCellPitchZ) + jitter.z};
}

} // namespace stratum::aquifer
