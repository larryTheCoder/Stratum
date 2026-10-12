// Stratum — what the aquifer changed, counted by kind.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The same chunk filled twice — once as the dimension declares, once with
// `aquifers_enabled` off, when the global picker decides every non-solid
// block: lava below lambda, the default fluid below `sea_level`, air above —
// differs exactly where the aquifer decided something the picker would not.
// That difference is the floor a probe case sets before it scores anything:
// a window the aquifer barely touches agrees with the server whatever the
// aquifer does, which is a vacuous pass rather than a measurement.
#pragma once

#include "support/fluid_flow.hpp"

#include <cstddef>
#include <cstdint>

namespace stratum::test {

struct AquiferFootprint {
    std::size_t total = 0;
    /// The picker's fluid, made solid: the barrier.
    std::size_t barrier = 0;
    /// The picker's fluid, made air: a dry cell.
    std::size_t dry = 0;
    /// The picker's water, made lava: a cell typed lava above lambda.
    std::size_t localLava = 0;
    /// At or above `sea_level`, where the picker has air: aquifer fluid or
    /// barrier above the sea — the regime a high preliminary surface opens.
    std::size_t aboveSea = 0;
    /// Anything else. The picker and the aquifer agree below lambda (Q2.4)
    /// and above `y_skip`, and the picker's air exists only above the sea, so
    /// this stays zero unless one of those stops holding.
    std::size_t other = 0;

    /// Counts one block whose category is @p picker with aquifers off and
    /// @p aquifer with them on, at height @p y.
    void add(const Category picker, const Category aquifer, const std::int32_t y,
             const std::int32_t seaLevel) {
        if (picker == aquifer) {
            return;
        }
        ++total;
        if (y >= seaLevel && picker == Category::Air) {
            ++aboveSea;
        } else if (isFluid(picker) && aquifer == Category::Solid) {
            ++barrier;
        } else if (isFluid(picker) && aquifer == Category::Air) {
            ++dry;
        } else if (picker == Category::Water && aquifer == Category::Lava) {
            ++localLava;
        } else {
            ++other;
        }
    }

    void add(const AquiferFootprint& more) {
        total += more.total;
        barrier += more.barrier;
        dry += more.dry;
        localLava += more.localLava;
        aboveSea += more.aboveSea;
        other += more.other;
    }
};

} // namespace stratum::test
