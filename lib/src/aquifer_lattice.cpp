// Stratum — the aquifer's cell lattice and fluid level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/lattice.hpp>
#include <stratum/javamath.hpp>

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

} // namespace stratum::aquifer
