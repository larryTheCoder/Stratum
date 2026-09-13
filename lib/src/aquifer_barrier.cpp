// Stratum — the aquifer's barrier sheets.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/aquifer/barrier.hpp>

#include <cmath>
#include <limits>

namespace stratum::aquifer {

namespace {

/// Q6.1's similarity, on squared distances: 1 at equality, decreasing as the
/// pair separates, zero at `kSimilarityRange`.
double similarity(const std::int64_t di, const std::int64_t dj) noexcept {
    return 1.0 - static_cast<double>(dj - di) / static_cast<double>(kSimilarityRange);
}

/// Q6.4's pressure function Π, level-difference branch: what two sources
/// push with when one of them reads fluid at `y` and the other air —
/// WHATEVER their types. Measured (barrier.hpp's own header): on the one
/// world with real water/lava junctions, this formula writes no stone the
/// server does not, on a pair of mixed type exactly as on a pair of one
/// type, while the constant in its place misses more and writes false
/// stone.
double levelPressure(const std::int32_t levelA, const std::int32_t levelB, const std::int32_t y,
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
    // `NAN` (`<cmath>`) is a `float` constant; assigning it here promotes
    // float to double under Clang's `-Wdouble-promotion` (-Werror in CI,
    // clang legs only — GCC does not flag it). `quiet_NaN<double>()` is
    // already the right type.
    double u = std::numeric_limits<double>::quiet_NaN();
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
    const double noiseTerm = (std::abs(u) <= 2.0) ? barrierNoise : 0.0;
    return 2.0 * (noiseTerm + u);
}

/// One term of Q6.6: does this pair, weighted by `weight` (either `s12`
/// alone, or `s12` doubled with `s13`/`s23`), push the block solid?
///
/// Q6.4's first clause, "one reads lava and the other water", is read as
/// what each source READS AT `y`, not as its type field — so it is the
/// case where BOTH read fluid here and the two fluids differ: a lava body
/// meeting a water body, which the server walls off with the constant
/// `kMixedTypePressure` and no level arithmetic. That is the one agreeing
/// pair that fires; both air, or both the same fluid, never does — and a
/// pair that disagrees at `y` takes the level formula whatever its types.
/// Each of those three choices is separately measured against the server
/// (barrier.hpp's own header).
bool termFires(const double density, const double weight, const BarrierSource& a,
               const BarrierSource& b, const std::int32_t y, const double barrierNoise) noexcept {
    const bool aFluid = y < a.level;
    const bool bFluid = y < b.level;
    if (aFluid && bFluid && a.type != b.type) {
        return (density + (weight * kMixedTypePressure)) > 0.0;
    }
    if (aFluid == bFluid) {
        return false;
    }
    return (density + (weight * levelPressure(a.level, b.level, y, barrierNoise))) > 0.0;
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
