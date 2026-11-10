// Stratum — old_blended_noise's floating-point forms, side by side, and the
// points where the server can tell them apart.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   stratum_blended_fpform_check <seed> <x0> <x1> <z0> <z1> <y0> <y1> [per-kind] [stride]
//
// The window is scanned every <stride> blocks in x and z (every y). Picks
// come in three kinds, <per-kind> of each (default 8): points where only the
// position form moves the value, only the selector form, and both to three
// or more distinct values.
//
// spec/blended-noise-spec.md's floating-point amendments (Q1.4 fp-form, Q1.5,
// Q2.1) state an evaluation order that differs from cubiomes' (which this
// build carried until engine v20) in three
// places, each of which can move the last bit:
//
//   A  the main sampler's position:   spec ((x*M)/F)*s, cubiomes (x*(M/F))*s
//   C  the main sampler's cap:        spec ((y*My)/Fy)*s, cubiomes y*((My/Fy)*s)
//      (and its divisor, spec ((My*k)/Fy)*s against ((My/Fy)*s)*k)
//   S  the selector:                  spec (main/10 + 1)/2, cubiomes 0.5 + 0.05*main
//
// Each choice is a bit, so every point has up to eight candidate values. This
// evaluates all eight from the same forty octave samplers BlendedNoise::modern
// builds (minimum, maximum, blend; one generator salted minecraft:terrain),
// checks that the all-spec variant reproduces BlendedNoise::sample
// exactly (the control: if it does not, nothing else here means anything),
// and then picks points whose candidates are not all equal. For each picked
// point it prints a tools/analysis/final-density-probe.sh target whose
// thresholds bisect the distinct candidates: each threshold T becomes one
// dimension where the block at the point is air iff the server's value is
// below T. Reading them back says which candidate the server computes, or
// that it computes none of them.
//
// A measuring instrument for one question (SPEC §11, "old_blended_noise's
// floating-point form"), not a feature.
#include <stratum/noise/blended.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

using stratum::noise::PerlinNoise;

constexpr double kBase = 684.412;
constexpr double kXzScale = 0.25;
constexpr double kYScale = 0.125;
constexpr double kXzFactor = 80.0;
constexpr double kYFactor = 160.0;
constexpr double kSmear = 8.0;

/// Q1.6: v - floor(v/2^25 + 0.5)*2^25, with a 64-bit floor.
double wrap(double v) {
    constexpr double kSpan = 33554432.0;
    return v -
           (static_cast<double>(static_cast<std::int64_t>(std::floor((v / kSpan) + 0.5))) * kSpan);
}

/// The sampler's cap convention: a negative cap never binds (Q2.2's
/// clamp_source is the fraction itself), which PerlinNoise::sample spells as
/// an infinite cap.
/// Bit equality, so +0.0 and -0.0 are two answers and the comparison says
/// what the server's threshold will see.
bool same(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

double capFor(double cap) {
    return cap < 0.0 ? std::numeric_limits<double>::infinity() : cap;
}

struct Samplers {
    std::vector<PerlinNoise> minimum;
    std::vector<PerlinNoise> maximum;
    std::vector<PerlinNoise> blend;
};

Samplers build(std::int64_t seed) {
    auto random = stratum::rng::XoroshiroPositionalFactory(seed).fromHashOf("minecraft:terrain");
    Samplers s;
    for (int i = 0; i < 16; ++i) {
        s.minimum.push_back(PerlinNoise::fromRandom(random));
    }
    for (int i = 0; i < 16; ++i) {
        s.maximum.push_back(PerlinNoise::fromRandom(random));
    }
    for (int i = 0; i < 8; ++i) {
        s.blend.push_back(PerlinNoise::fromRandom(random));
    }
    return s;
}

/// Bit 0 = A (spec position), bit 1 = C (spec cap and divisor), bit 2 = S
/// (spec selector). Variant 0 is cubiomes' order; variant 7 is the spec's, which
/// BlendedNoise::sample computes since engine v20 (the control below).
double evaluate(const Samplers& s, unsigned variant, double x, double y, double z) {
    const bool specA = (variant & 1U) != 0;
    const bool specC = (variant & 2U) != 0;
    const bool specS = (variant & 4U) != 0;
    const double mxz = kBase * kXzScale;
    const double my = kBase * kYScale;
    const double xzStep = mxz / kXzFactor;
    const double yStep = my / kYFactor;
    const double fx = (x * mxz) / kXzFactor;
    const double fy = (y * my) / kYFactor;
    const double fz = (z * mxz) / kXzFactor;
    const double smearF = (my * kSmear) / kYFactor;

    double main = 0.0;
    double scale = 1.0;
    for (int n = 0; n < 8; ++n) {
        const double ax = specA ? wrap(fx * scale) : wrap(x * xzStep * scale);
        const double ay = specA ? wrap(fy * scale) : wrap(y * yStep * scale);
        const double az = specA ? wrap(fz * scale) : wrap(z * xzStep * scale);
        const double divisor = specC ? smearF * scale : (yStep * scale) * kSmear;
        double cap = fy * scale;
        if (!specC) {
            cap = y < 0.0 ? -1.0 : y * (yStep * scale);
        }
        main +=
            s.blend[static_cast<std::size_t>(n)].sample(ax, ay, az, divisor, capFor(cap)) / scale;
        scale *= 0.5;
    }
    const double selector = specS ? ((main / 10.0) + 1.0) / 2.0 : 0.5 + (0.05 * main);
    const bool skipMin = selector >= 1.0;
    const bool skipMax = selector <= 0.0;

    double lo = 0.0;
    double hi = 0.0;
    scale = 1.0;
    const double px = x * mxz;
    const double py = y * my;
    const double pz = z * mxz;
    const double smear = my * kSmear;
    for (int n = 0; n < 16; ++n) {
        const double ax = wrap(px * scale);
        const double ay = wrap(py * scale);
        const double az = wrap(pz * scale);
        const double divisor = smear * scale;
        const double cap = capFor(py * scale);
        if (!skipMin) {
            lo += s.minimum[static_cast<std::size_t>(n)].sample(ax, ay, az, divisor, cap) / scale;
        }
        if (!skipMax) {
            hi += s.maximum[static_cast<std::size_t>(n)].sample(ax, ay, az, divisor, cap) / scale;
        }
        scale *= 0.5;
    }
    const double a = lo / 512.0;
    const double b = hi / 512.0;
    double r = 0.0;
    if (selector < 0.0) {
        r = a;
    } else if (selector > 1.0) {
        r = b;
    } else {
        r = a + (selector * (b - a));
    }
    return r / 128.0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 8) {
        std::fprintf(stderr, "usage: %s <seed> <x0> <x1> <z0> <z1> <y0> <y1> [per-kind] [stride]\n",
                     argv[0]);
        return 2;
    }
    const std::int64_t seed = std::atoll(argv[1]);
    const int x0 = std::atoi(argv[2]);
    const int x1 = std::atoi(argv[3]);
    const int z0 = std::atoi(argv[4]);
    const int z1 = std::atoi(argv[5]);
    const int y0 = std::atoi(argv[6]);
    const int y1 = std::atoi(argv[7]);
    const int perKind = argc > 8 ? std::atoi(argv[8]) : 8;
    const int stride = argc > 9 ? std::max(1, std::atoi(argv[9])) : 1;

    const Samplers s = build(seed);
    const auto reference =
        stratum::noise::BlendedNoise::modern(seed, {.xzScale = kXzScale,
                                                    .yScale = kYScale,
                                                    .xzFactor = kXzFactor,
                                                    .yFactor = kYFactor,
                                                    .smearScaleMultiplier = kSmear});

    std::uint64_t points = 0;
    std::uint64_t controlMiss = 0;
    std::array<std::uint64_t, 8> differsFromBuild{};

    struct Pick {
        int x, y, z;
        std::array<double, 8> v;
    };

    std::vector<Pick> picks;
    std::array<int, 3> taken{};
    for (int x = x0; x <= x1; x += stride) {
        for (int z = z0; z <= z1; z += stride) {
            for (int y = y0; y <= y1; ++y) {
                const double dx = x;
                const double dy = y;
                const double dz = z;
                std::array<double, 8> v{};
                for (unsigned k = 0; k < 8; ++k) {
                    v[k] = evaluate(s, k, dx, dy, dz);
                }
                ++points;
                if (!same(v[7], reference.sample(dx, dy, dz))) {
                    ++controlMiss;
                }
                for (unsigned k = 1; k < 8; ++k) {
                    differsFromBuild[k] += same(v[k], v[0]) ? 0U : 1U;
                }
                // Kind 0: the position form alone moves it; 1: the selector
                // form alone; 2: both, to three distinct values. One pick
                // per kind per x column, so they spread over the window.
                const bool movesA = !same(v[1], v[0]);
                const bool movesS = !same(v[4], v[0]);
                const bool three = !same(v[5], v[1]) && !same(v[5], v[4]) && movesA && movesS;
                int kind = -1;
                if (three) {
                    kind = 2;
                } else if (movesA && !movesS) {
                    kind = 0;
                } else if (movesS && !movesA) {
                    kind = 1;
                }
                if (kind >= 0 && taken[static_cast<std::size_t>(kind)] < perKind &&
                    (picks.empty() || picks.back().x != x)) {
                    ++taken[static_cast<std::size_t>(kind)];
                    picks.push_back({x, y, z, v});
                }
            }
        }
    }
    std::fprintf(stderr, "points %llu, control misses (variant 7 vs BlendedNoise) %llu\n",
                 static_cast<unsigned long long>(points),
                 static_cast<unsigned long long>(controlMiss));
    for (unsigned k = 1; k < 8; ++k) {
        std::fprintf(stderr, "variant %u (A%d C%d S%d) differs from cubiomes' order at %llu\n", k,
                     static_cast<int>(k & 1U), static_cast<int>((k >> 1U) & 1U),
                     static_cast<int>((k >> 2U) & 1U),
                     static_cast<unsigned long long>(differsFromBuild[k]));
    }

    // The targets file: per point, thresholds at every distinct candidate
    // and one ulp above the largest — so the reading pins the server's value
    // to one candidate, or shows it below or above them all.
    std::printf("[\n");
    for (std::size_t i = 0; i < picks.size(); ++i) {
        const Pick& p = picks[i];
        std::vector<double> distinct(p.v.begin(), p.v.end());
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end(), same), distinct.end());
        std::printf("  {\"x\": %d, \"y\": %d, \"z\": %d, \"function\": "
                    "\"minecraft:overworld/base_3d_noise\", \"candidates\": [",
                    p.x, p.y, p.z);
        for (unsigned k = 0; k < 8; ++k) {
            std::printf("%s%.17g", k == 0 ? "" : ", ", p.v[k]);
        }
        std::printf("], \"thresholds\": [");
        for (std::size_t j = 0; j < distinct.size(); ++j) {
            std::printf("%s%.17g", j == 0 ? "" : ", ", distinct[j]);
        }
        std::printf(", %.17g]}%s\n",
                    std::nextafter(distinct.back(), std::numeric_limits<double>::infinity()),
                    i + 1 == picks.size() ? "" : ",");
    }
    std::printf("]\n");
    return controlMiss == 0 ? 0 : 1;
}
