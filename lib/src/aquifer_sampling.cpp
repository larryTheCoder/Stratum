// Stratum — where the aquifer reads its router inputs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/sampling.hpp>

namespace stratum::aquifer {

SamplePos spreadSample(const CellIndex cell, const CellIndex centre) noexcept {
    return SamplePos{.x = cell.x, .y = levelBand(centre.y), .z = cell.z};
}

} // namespace stratum::aquifer
