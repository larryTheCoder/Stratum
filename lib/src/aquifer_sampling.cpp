// Stratum — where the aquifer reads its router inputs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/sampling.hpp>

namespace stratum::aquifer {

SamplePos spreadSample(const CellIndex cell, const CellIndex centre) noexcept {
    return SamplePos{.x = cell.x, .y = levelBand(centre.y), .z = cell.z};
}

SamplePos lavaSample(const CellIndex centre) noexcept {
    // Spelled from the CENTRE on all three axes, not from the cell index. For
    // y that is `levelBand`, which the spread shares. For x and z it cannot be
    // the cell index at all: the cell pitch is 16 and this lattice's is 64, so
    // there is no cell index to reuse.
    return SamplePos{.x = javamath::floorDiv(centre.x, kLavaIndexPitchXZ),
                     .y = javamath::floorDiv(centre.y, kLavaIndexPitchY),
                     .z = javamath::floorDiv(centre.z, kLavaIndexPitchXZ)};
}

} // namespace stratum::aquifer
