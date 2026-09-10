// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/barrier.hpp>

#include <cmath>

namespace stratum::aquifer {

namespace {

/// Q6.1's similarity, on squared distances: 1 at equality, decreasing as the
/// pair separates, zero at `kSimilarityRange`.
double similarity(const std::int64_t di, const std::int64_t dj) noexcept {
    return 1.0 - static_cast<double>(dj - di) / static_cast<double>(kSimilarityRange);
}

/// Q6.4's pressure function Π, same-fluid-type branch. The mixed
/// water/lava branch (`Π = 2.0`) is not implemented: this build's fluid TYPE
/// integration is a separate open question (this file's own header), and no
/// probe has driven `lava` enough to tell the two apart yet.
double pressure(const std::int32_t levelA, const std::int32_t levelB, const std::int32_t y,
                const double barrierNoise) noexcept {
    const std::int32_t deltaInt = levelA < levelB ? levelB - levelA : levelA - levelB;
    if (deltaInt == 0) {
        return 0.0;
    }
    const auto delta = static_cast<double>(deltaInt);
    const double m = (static_cast<double>(levelA) + static_cast<double>(levelB)) / 2.0;
    const double h = static_cast<double>(y) + 0.5 - m;
    const double r = delta / 2.0;
    // `std::abs` is not constexpr for doubles before C++23; this is not a
    // constexpr context, so it is used directly rather than a ternary.
    const double t = r - std::abs(h);
    double u = NAN;
    if (h > 0) {
        u = (t > 0) ? (t / 1.5) : (t / 2.5);
    } else {
        const double threeT = 3.0 + t;
        // The "/10" branch below: never reached by a real third-source pair
        // in either of this project's two barrier-probe seeds (this file's
        // own header) — implemented as the clean-room spec states, inert
        // until a configuration that exercises it is found.
        u = (threeT > 0) ? (threeT / 3.0) : (threeT / 10.0);
    }
    const double b = (std::abs(u) <= 2.0) ? barrierNoise : 0.0;
    return 2.0 * (b + u);
}

/// One term of Q6.6: does this pair, weighted by `weight` (either `s12`
/// alone, or `s12` doubled with `s13`/`s23`), push the block solid? A pair
/// that agrees at `y` never fires — see barrier.hpp's own doc on
/// `placesBarrier`.
bool termFires(const double density, const double weight, const BarrierSource& a,
               const BarrierSource& b, const std::int32_t y, const double barrierNoise) noexcept {
    if ((y < a.level) == (y < b.level)) {
        return false;
    }
    return (density + (weight * pressure(a.level, b.level, y, barrierNoise))) > 0.0;
}

} // namespace

bool placesBarrier(const BarrierAt& at) noexcept {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    if (s12 <= 0.0) {
        return false;
    }
    if (termFires(at.density, s12, at.nearest, at.second, at.y, at.barrier)) {
        return true;
    }
    const double s13 = similarity(at.nearest.distanceSq, at.third.distanceSq);
    if (s13 > 0.0 && termFires(at.density, s12 * s13, at.nearest, at.third, at.y, at.barrier)) {
        return true;
    }
    const double s23 = similarity(at.second.distanceSq, at.third.distanceSq);
    return s23 > 0.0 && termFires(at.density, s12 * s23, at.second, at.third, at.y, at.barrier);
}

} // namespace stratum::aquifer
