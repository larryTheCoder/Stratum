// Stratum — Perlin, octave, normal and simplex noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// REFERENCE (SPEC §2, CLAUDE.md): the composition implemented here — the
// gradient set, the octave salts, the persistence and lacunarity schedules
// and the normal-noise scaling — was checked against cubiomes
// (https://github.com/Cubitect/cubiomes, MIT, Cubitect), file noise.c, which
// SPEC §2 names as the reference for noise and biome math. The known-answer
// vectors in tests/unit/noise_vectors.inc are produced from it.
//
// This file depends on -ffp-contract=off (SPEC §5.4): the interpolations
// below are sums of products, and letting the compiler contract them into
// FMAs changes the result.
//
// One limit of that oracle is worth knowing: cubiomes precomputes the MD5
// octave salts for octave_-12 through octave_0 only, and indexes that table
// directly, so a configuration reaching octave_1 reads past the end of it.
// Vanilla hashes the name at run time and has no such limit, and neither
// does the code below — which means positive octave indices are correct here
// but cannot be covered by cubiomes vectors.

#include <stratum/hash/md5.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace stratum::noise {

namespace {

/// The 16 gradient directions of improved Perlin noise. Twelve distinct
/// edges of a cube, with four repeated to make the index a cheap mask.
constexpr std::array<double, 16> kGradientX = {1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0,  -1.0,
                                               0.0, 0.0,  0.0, 0.0,  1.0, 0.0,  -1.0, 0.0};
constexpr std::array<double, 16> kGradientY = {1.0, 1.0,  -1.0, -1.0, 0.0, 0.0,  0.0, 0.0,
                                               1.0, -1.0, 1.0,  -1.0, 1.0, -1.0, 1.0, -1.0};
constexpr std::array<double, 16> kGradientZ = {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, -1.0, -1.0,
                                               1.0, 1.0, -1.0, -1.0, 0.0, 1.0, 0.0,  -1.0};

[[nodiscard]] double gradientDot(std::uint8_t index, double x, double y, double z) noexcept {
    const std::size_t slot = index & 0x0FU;
    return (kGradientX[slot] * x) + (kGradientY[slot] * y) + (kGradientZ[slot] * z);
}

/// Perlin's quintic fade, 6t^5 - 15t^4 + 10t^3, written the way it is
/// evaluated rather than the way it is written down: the nesting is part of
/// the result.
[[nodiscard]] double fade(double t) noexcept {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

[[nodiscard]] double lerp(double t, double a, double b) noexcept {
    return a + (t * (b - a));
}

/// Exactly `value == 0.0`, including -0.0, without a float comparison. These
/// are genuine exact-zero branches in the algorithm — "this octave is
/// absent", "y is not folded" — not tolerance tests, and the project keeps
/// -Wfloat-equal on.
[[nodiscard]] bool isZero(double value) noexcept {
    return (std::bit_cast<std::uint64_t>(value) & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0U;
}

/// The contribution of one simplex corner, zero outside its radius.
[[nodiscard]] double simplexCorner(std::uint8_t gradient, double x, double y, double z,
                                   double falloff) noexcept {
    double contribution = falloff - (x * x) - (y * y) - (z * z);
    if (contribution < 0.0) {
        return 0.0;
    }
    contribution *= contribution;
    return contribution * contribution * gradientDot(gradient, x, y, z);
}

} // namespace

PerlinNoise::PerlinNoise(double originX, double originY, double originZ,
                         std::array<std::uint8_t, 256> permutation) noexcept
    : originX_(originX), originY_(originY), originZ_(originZ), permutation_(permutation) {}

namespace {

/// The construction both generators share: three origin draws, then a
/// Fisher-Yates shuffle over the shrinking tail. Templated on the generator
/// rather than duplicated, because the whole point is that the *order* is
/// the same and only the source of the numbers differs.
template<typename Random>
[[nodiscard]] PerlinNoise perlinFrom(Random& random) {
    // Order matters: three origin draws, then the shuffle. Reordering them
    // silently changes every octave seeded from this generator.
    const double originX = random.nextDouble() * 256.0;
    const double originY = random.nextDouble() * 256.0;
    const double originZ = random.nextDouble() * 256.0;

    std::array<std::uint8_t, 256> permutation{};
    for (std::size_t i = 0; i < permutation.size(); ++i) {
        permutation[i] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t i = 0; i < permutation.size(); ++i) {
        const auto swap =
            static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(256U - i))) + i;
        std::swap(permutation[i], permutation[swap]);
    }

    return PerlinNoise{originX, originY, originZ, permutation};
}

} // namespace

PerlinNoise PerlinNoise::fromRandom(rng::JavaRandom& random) {
    return perlinFrom(random);
}

PerlinNoise PerlinNoise::fromRandom(rng::Xoroshiro128PlusPlus& random) {
    return perlinFrom(random);
}

double PerlinNoise::sample(double x, double y, double z) const noexcept {
    return sample(x, y, z, 0.0, 0.0);
}

double PerlinNoise::sample(double x, double y, double z, double yScale,
                           double yMax) const noexcept {
    const double shiftedX = x + originX_;
    const double shiftedY = y + originY_;
    const double shiftedZ = z + originZ_;

    const double cellX = std::floor(shiftedX);
    const double cellY = std::floor(shiftedY);
    const double cellZ = std::floor(shiftedZ);

    const double localX = shiftedX - cellX;
    double localY = shiftedY - cellY;
    const double localZ = shiftedZ - cellZ;

    const auto latticeX = static_cast<std::uint8_t>(static_cast<std::int32_t>(cellX));
    const auto latticeY = static_cast<std::uint8_t>(static_cast<std::int32_t>(cellY));
    const auto latticeZ = static_cast<std::uint8_t>(static_cast<std::int32_t>(cellZ));

    const double fadeX = fade(localX);
    const double fadeY = fade(localY);
    const double fadeZ = fade(localZ);

    if (!isZero(yScale)) {
        // Fold y back onto a slab: the interpolation weight keeps its full
        // fade, while the gradient sees the folded offset.
        const double clamped = (yMax < localY) ? yMax : localY;
        localY -= std::floor(clamped / yScale) * yScale;
    }

    const auto cornerA = static_cast<std::uint8_t>(at(latticeX) + latticeY);
    const auto cornerB = static_cast<std::uint8_t>(at(latticeX + 1) + latticeY);

    const auto cornerAA = static_cast<std::uint8_t>(at(cornerA) + latticeZ);
    const auto cornerBA = static_cast<std::uint8_t>(at(cornerB) + latticeZ);
    const auto cornerAB = static_cast<std::uint8_t>(at(cornerA + 1) + latticeZ);
    const auto cornerBB = static_cast<std::uint8_t>(at(cornerB + 1) + latticeZ);

    const double x0y0z0 = gradientDot(at(cornerAA), localX, localY, localZ);
    const double x1y0z0 = gradientDot(at(cornerBA), localX - 1.0, localY, localZ);
    const double x0y1z0 = gradientDot(at(cornerAB), localX, localY - 1.0, localZ);
    const double x1y1z0 = gradientDot(at(cornerBB), localX - 1.0, localY - 1.0, localZ);
    const double x0y0z1 = gradientDot(at(cornerAA + 1), localX, localY, localZ - 1.0);
    const double x1y0z1 = gradientDot(at(cornerBA + 1), localX - 1.0, localY, localZ - 1.0);
    const double x0y1z1 = gradientDot(at(cornerAB + 1), localX, localY - 1.0, localZ - 1.0);
    const double x1y1z1 = gradientDot(at(cornerBB + 1), localX - 1.0, localY - 1.0, localZ - 1.0);

    const double y0z0 = lerp(fadeX, x0y0z0, x1y0z0);
    const double y1z0 = lerp(fadeX, x0y1z0, x1y1z0);
    const double y0z1 = lerp(fadeX, x0y0z1, x1y0z1);
    const double y1z1 = lerp(fadeX, x0y1z1, x1y1z1);

    return lerp(fadeZ, lerp(fadeY, y0z0, y1z0), lerp(fadeY, y0z1, y1z1));
}

double PerlinNoise::sampleSimplex2D(double x, double y) const noexcept {
    const double kSkew = 0.5 * (std::sqrt(3.0) - 1.0);
    const double kUnskew = (3.0 - std::sqrt(3.0)) / 6.0;

    const double skewed = (x + y) * kSkew;
    const auto cellX = static_cast<std::int32_t>(std::floor(x + skewed));
    const auto cellY = static_cast<std::int32_t>(std::floor(y + skewed));

    const double unskewed = static_cast<double>(cellX + cellY) * kUnskew;
    const double x0 = x - (static_cast<double>(cellX) - unskewed);
    const double y0 = y - (static_cast<double>(cellY) - unskewed);

    // Which of the two triangles in the skewed cell the point falls in.
    const int stepX = (x0 > y0) ? 1 : 0;
    const int stepY = 1 - stepX;

    const double x1 = (x0 - stepX) + kUnskew;
    const double y1 = (y0 - stepY) + kUnskew;
    const double x2 = (x0 - 1.0) + (2.0 * kUnskew);
    const double y2 = (y0 - 1.0) + (2.0 * kUnskew);

    const std::uint8_t rowA = at(cellY);
    const std::uint8_t rowB = at(cellY + stepY);
    const std::uint8_t rowC = at(cellY + 1);

    const std::uint8_t gradientA = at(rowA + cellX);
    const std::uint8_t gradientB = at(rowB + cellX + stepX);
    const std::uint8_t gradientC = at(rowC + cellX + 1);

    double total = 0.0;
    total += simplexCorner(static_cast<std::uint8_t>(gradientA % 12U), x0, y0, 0.0, 0.5);
    total += simplexCorner(static_cast<std::uint8_t>(gradientB % 12U), x1, y1, 0.0, 0.5);
    total += simplexCorner(static_cast<std::uint8_t>(gradientC % 12U), x2, y2, 0.0, 0.5);
    return 70.0 * total;
}

OctaveNoise OctaveNoise::create(rng::Xoroshiro128PlusPlus& random, int firstOctave,
                                std::span<const double> amplitudes) {
    OctaveNoise octaveNoise;

    // Exact powers of two, so no rounding creeps into the frequency schedule.
    double frequency = std::ldexp(1.0, firstOctave);
    // 2^(n-1) / (2^n - 1): the persistence that makes the amplitudes sum to
    // something close to one whatever the octave count.
    const auto count = static_cast<int>(amplitudes.size());
    double persistence =
        count == 0 ? 0.0 : std::ldexp(1.0, count - 1) / (std::ldexp(1.0, count) - 1.0);

    // Two draws taken once, then XORed with a per-octave salt. This is what
    // lets an octave be added or removed without disturbing the others.
    const auto baseLo = static_cast<std::uint64_t>(random.nextLong());
    const auto baseHi = static_cast<std::uint64_t>(random.nextLong());

    for (std::size_t i = 0; i < amplitudes.size(); ++i) {
        if (!isZero(amplitudes[i])) {
            const std::string name = "octave_" + std::to_string(firstOctave + static_cast<int>(i));
            const rng::Seed128 salt = rng::seedFromHashOf(name);
            rng::Xoroshiro128PlusPlus octaveRandom{
                rng::Seed128{.lo = baseLo ^ salt.lo, .hi = baseHi ^ salt.hi}};

            octaveNoise.octaves_.push_back(Octave{.noise = PerlinNoise::fromRandom(octaveRandom),
                                                  .amplitude = amplitudes[i] * persistence,
                                                  .frequency = frequency});
        }
        // Advanced even for a skipped octave: a zero amplitude means "this
        // frequency contributes nothing", not "this frequency is not there".
        frequency *= 2.0;
        persistence *= 0.5;
    }

    return octaveNoise;
}

double OctaveNoise::sample(double x, double y, double z) const noexcept {
    double total = 0.0;
    for (const Octave& octave : octaves_) {
        total += octave.amplitude * octave.noise.sample(x * octave.frequency, y * octave.frequency,
                                                        z * octave.frequency);
    }
    return total;
}

NormalNoise NormalNoise::create(rng::Xoroshiro128PlusPlus& random, int firstOctave,
                                std::span<const double> amplitudes) {
    // Sequenced through named locals, not passed as two constructor
    // arguments: both calls draw from the same generator, so which one runs
    // first is part of the result. C++ leaves the evaluation order of
    // constructor arguments unspecified, and MSVC evaluates them right to
    // left — which silently swapped the two stacks and produced a different
    // world on Windows while every other platform agreed.
    OctaveNoise firstStack = OctaveNoise::create(random, firstOctave, amplitudes);
    OctaveNoise secondStack = OctaveNoise::create(random, firstOctave, amplitudes);
    NormalNoise noise{std::move(firstStack), std::move(secondStack), 1.0};

    // Leading and trailing zero amplitudes do not count toward the scaling:
    // they are padding that positions the rest, not octaves in their own
    // right.
    std::size_t first = 0;
    std::size_t last = amplitudes.size();
    while (last > first && isZero(amplitudes[last - 1])) {
        --last;
    }
    while (first < last && isZero(amplitudes[first])) {
        ++first;
    }

    const auto effective = static_cast<double>(last - first);
    // (5/3) * n / (n + 1), as a single division so it rounds the same way
    // the reference's literals do.
    noise.valueFactor_ = (5.0 * effective) / (3.0 * (effective + 1.0));
    return noise;
}

double NormalNoise::sample(double x, double y, double z) const noexcept {
    // The second stack is sampled at 337/331 of the frequency, a ratio close
    // enough to one to correlate and irrational enough not to repeat.
    constexpr double kSecondFrequency = 337.0 / 331.0;
    const double combined =
        first_.sample(x, y, z) +
        second_.sample(x * kSecondFrequency, y * kSecondFrequency, z * kSecondFrequency);
    return combined * valueFactor_;
}

} // namespace stratum::noise
