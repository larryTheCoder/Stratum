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
/// push with whenever their levels differ — whatever they read at `y` and
/// whatever their types, the one exception being the mixed-type pair that
/// `termFires` takes the constant for. Measured (barrier.hpp's own header):
/// on the one world with real water/lava junctions, this formula writes no
/// stone the server does not, on a pair of mixed type exactly as on a pair
/// of one type, while the constant in its place misses more and writes
/// false stone.
///
/// ALL FOUR DIVISORS ARE NOW MEASURED, which they were not while this
/// function was reached only by a pair that DISAGREED at `y`. On that
/// domain `t >= 0.5` for every integer `(L_A, L_B, y)` — a disagreement
/// means `min(L) <= y < max(L)`, so `|h| <= r - 0.5` — which put the `t <= 0`
/// and `3 + t <= 0` arms out of reach by arithmetic rather than by accident.
/// With the pair reading the SAME thing admitted (as Q6.4 always said), the
/// four arms partition cleanly and each is a different part of the sheet:
///
///   * `h > 0`, `t > 0`  -> /1.5: the pair disagrees, above the midpoint.
///   * `h > 0`, `t <= 0` -> /2.5: the barrier's LID. Reached only by a pair
///     that both read AIR, where `t = max(L) - y - 0.5`, so it is the few
///     blocks just above the higher of the two levels.
///   * `h <= 0`, `3 + t > 0` -> /3: the pair disagrees, below the midpoint,
///     or reads fluid within three blocks under the lower level.
///   * `h <= 0`, `3 + t <= 0` -> /10: the barrier's FLOOR. Reached only by a
///     pair that both read the SAME fluid, where `t = y + 0.5 - min(L)`, so
///     it is everything four or more blocks below the lower of the two.
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
        // The "/10" branch below is MEASURED, on 96 292 blocks it alone
        // decides across three seeds of `aquifer-deepfloor-probe.sh`. It
        // beats /3 on 65 231 of 65 231 blocks where the two disagree —
        // every one of them server stone — and the bracket around it is
        // two-sided and monotone: 9.9 leaves 937 of the server's barriers
        // unwritten, 10.1 writes 872 blocks of stone the server does not,
        // and 10 itself is exact. See barrier.hpp's own header.
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
/// `kMixedTypePressure` and no level arithmetic. Every OTHER pair takes the
/// level formula, whatever its types and whatever it reads — Q6.4 gates
/// nothing else, and `levelPressure`'s own `Δ = 0 -> Π = 0` is what makes a
/// pair of equal level inert without a guard in front of it.
///
/// THERE USED TO BE SUCH A GUARD HERE — `aFluid == bFluid` returned false —
/// and the server refutes it. It was this build's own addition, with no
/// counterpart in the spec, and it suppressed two of Π's four arms outright
/// (see `levelPressure`). Scored block by block against the server on the
/// `aquifer-deepfloor-probe.sh` worlds, 110 097 250 blocks over three seeds
/// and seven dimensions, against 4 982 316 blocks of server stone:
///
///     guarded (both air and both fluid)   480 354 misses   0 false stone
///     guard lifted for both-air only      457 970 misses   0 false stone
///     guard lifted for both-fluid only     22 384 misses   0 false stone
///     no guard (Q6.4 as written)                0 misses   0 false stone
///
/// and on the four worlds that predate this question — `barrier3way` plus
/// the three water/lava seeds, 90 027 838 blocks against 243 887 server
/// stone — the same ordering holds at 161 / 46 / 154 / 39 misses. No model
/// writes a block of stone the server does not on `barrier3way`; the 560
/// that the water/lava worlds show are IDENTICAL under all four models and
/// sit entirely in their `sea70` arms, where this bare predicate is scored
/// over rows that Q2.4 and Q6.3 own in front of it — a floor of the scoring
/// harness, not a disagreement between the models. The abort condition set
/// in advance was "the un-gated reading writes false stone the guarded one
/// does not"; it never fired on any world, at any density, on any seed.
bool termFires(const double density, const double weight, const BarrierSource& a,
               const BarrierSource& b, const std::int32_t y, const double barrierNoise) noexcept {
    const bool aFluid = y < a.level;
    const bool bFluid = y < b.level;
    if (aFluid && bFluid && a.type != b.type) {
        return (density + (weight * kMixedTypePressure)) > 0.0;
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
