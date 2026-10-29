// Stratum — ore veins: the richness gate worked out once a height.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ore::RichnessGate is clearsRichness with what `y` alone decides made once,
// for the filler, which asks about every solid block of a height at that
// height. clearsRichness is written through it now, so the two cannot part;
// what this pins is that the gate is still the documented rule (ore/vein.hpp,
// "THE DERIVATION"), written out again here from that text, at every height
// either side of both ranges and at toggles on each side of every threshold,
// NaN and both zeros included — plus hand-worked answers at the edges.
#include <stratum/ore/vein.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

/// The rule as vein.hpp states it: copper in y 0..50 with a positive toggle,
/// iron in y -60..-8 with one at most 0, and |toggle| at least 0.6 at either
/// end of the metal's range, falling by 0.2 over the 20 blocks inside it.
[[nodiscard]] bool documented(const std::int32_t y, const double toggle) {
    const bool copperRange = y >= 0 && y <= 50;
    const bool ironRange = y >= -60 && y <= -8;
    bool candidate = false;
    std::int32_t fromEnd = 0;
    if (copperRange) {
        candidate = toggle > 0.0;
        fromEnd = std::min(y, 50 - y);
    } else if (ironRange) {
        candidate = toggle <= 0.0;
        fromEnd = std::min(y + 60, -8 - y);
    }
    if (!candidate) {
        return false;
    }
    const double required = 0.6 - (0.2 * std::min(fromEnd, 20) / 20.0);
    return std::abs(toggle) >= required;
}

} // namespace

TEST_CASE("the richness gate of a height is clearsRichness at that height",
          "[ore][vein][richness]") {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    std::size_t compared = 0;
    std::size_t differing = 0;
    std::size_t cleared = 0;
    for (std::int32_t y = -70; y <= 60; ++y) {
        const stratum::ore::RichnessGate gate(y);
        std::vector<double> toggles{kNaN, -kNaN, 0.0, -0.0, kInfinity, -kInfinity, 1.0,  -1.0,
                                    0.5,  -0.5,  0.4, -0.4, 0.6,       -0.6,       0.45, -0.45};
        // Each side of this height's threshold, for either metal's sign.
        for (const std::int32_t fromEnd : {0, 5, 10, 19, 20, 25}) {
            const double required = 0.6 - (0.2 * std::min(fromEnd, 20) / 20.0);
            for (const double edge :
                 {required, std::nextafter(required, 0.0), std::nextafter(required, 1.0)}) {
                toggles.push_back(edge);
                toggles.push_back(-edge);
            }
        }
        for (const double toggle : toggles) {
            ++compared;
            const bool viaGate = gate.clears(toggle);
            differing += viaGate != documented(y, toggle) ? 1U : 0U;
            differing += viaGate != stratum::ore::clearsRichness(y, toggle) ? 1U : 0U;
            cleared += viaGate ? 1U : 0U;
        }
    }
    CHECK(differing == 0U);
    CHECK(compared > 5000U);
    CHECK(cleared > 500U);
}

TEST_CASE("the richness gate at the ends of both ranges and in the dead zone",
          "[ore][vein][richness]") {
    // Copper's ends need 0.6, positive; 20 blocks in, 0.6 - 0.2, the double
    // 0.39999999999999997, which 0.4 clears.
    CHECK(stratum::ore::RichnessGate(0).clears(0.6));
    CHECK_FALSE(stratum::ore::RichnessGate(0).clears(std::nextafter(0.6, 0.0)));
    CHECK_FALSE(stratum::ore::RichnessGate(0).clears(-0.9));
    CHECK(stratum::ore::RichnessGate(50).clears(0.6));
    CHECK(stratum::ore::RichnessGate(25).clears(0.4));
    CHECK(stratum::ore::RichnessGate(25).clears(0.6 - 0.2));
    CHECK_FALSE(stratum::ore::RichnessGate(25).clears(std::nextafter(0.6 - 0.2, 0.0)));
    // Iron's: at most 0, -8 inclusive; 0.0 is iron's sign, but not rich.
    CHECK(stratum::ore::RichnessGate(-8).clears(-0.6));
    CHECK_FALSE(stratum::ore::RichnessGate(-8).clears(0.7));
    CHECK(stratum::ore::RichnessGate(-60).clears(-1.0));
    CHECK_FALSE(stratum::ore::RichnessGate(-30).clears(0.0));
    CHECK_FALSE(stratum::ore::RichnessGate(-61).clears(-1.0));
    // The dead zone between them, and past either end, clears nothing.
    for (const std::int32_t y : {-7, -4, -1, 51, 320, -64}) {
        CHECK_FALSE(stratum::ore::RichnessGate(y).clears(1.0));
        CHECK_FALSE(stratum::ore::RichnessGate(y).clears(-1.0));
    }
    // NaN is no candidate of either metal.
    CHECK_FALSE(stratum::ore::RichnessGate(25).clears(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(stratum::ore::RichnessGate(-30).clears(std::numeric_limits<double>::quiet_NaN()));
}
