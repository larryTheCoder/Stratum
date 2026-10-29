// Stratum — the noise samplers against their own previous implementation.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS IS FOR. The noise samplers were rewritten for speed (performance
// round 3) under one rule: every double the same as before, to the bit. The
// cubiomes vectors in noise_test.cpp and blended_test.cpp pin a few hundred
// points; this file pins millions, by running a VERBATIM COPY of the
// implementation that stood before the rewrite (namespace `base` below, from
// lib/src/perlin.cpp, blended.cpp and end_islands.cpp at 5507ed1) beside the
// library's current one, on the same inputs, and comparing the results'
// bytes. A copy rather than a capture, so that the inputs can be random and
// many, and so that the reference cannot drift with the code it checks.
//
// The copy is the old code with its member accesses spelled through the
// public accessors (`originX_` as `noise.originX()`, `at(i)` as a lambda over
// `noise.permutation()`), and its private octave lists rebuilt by copies of
// the constructions that fill them; nothing in the arithmetic is touched.
// The rebuilt constructions are checked by the comparison itself: a stack
// built differently would sample differently.
//
// The inputs aim at where a rewrite of floor, the lattice and the fold could
// go wrong: negative coordinates, exact integers and half-integers and their
// neighbours one ulp away, the signed zeros, magnitudes on both sides of
// 2^24 and 2^25 (maintainPrecision's fold) and out to the int32 lattice
// limit, cells whose low byte is 0, 1, 254 or 255 (every wrap of the
// permutation table), and a fold whose scale and cap run through zero, the
// subnormals, the infinities and NaN.

#include <stratum/javamath.hpp>
#include <stratum/noise/blended.hpp>
#include <stratum/noise/end_islands.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::noise::BlendedNoise;
using stratum::noise::EndIslands;
using stratum::noise::NormalNoise;
using stratum::noise::OctaveNoise;
using stratum::noise::PerlinNoise;
using stratum::rng::JavaRandom;
using stratum::rng::Xoroshiro128PlusPlus;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// ---------------------------------------------------------------------------
// The previous implementation, verbatim but for member access (see the top).
// ---------------------------------------------------------------------------
namespace base {

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

[[nodiscard]] double fade(double t) noexcept {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

[[nodiscard]] double lerp(double t, double a, double b) noexcept {
    return a + (t * (b - a));
}

[[nodiscard]] bool isZero(double value) noexcept {
    return (std::bit_cast<std::uint64_t>(value) & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0U;
}

[[nodiscard]] double simplexCorner(std::uint8_t gradient, double x, double y, double z,
                                   double falloff) noexcept {
    double contribution = falloff - (x * x) - (y * y) - (z * z);
    if (contribution < 0.0) {
        return 0.0;
    }
    contribution *= contribution;
    return contribution * contribution * gradientDot(gradient, x, y, z);
}

[[nodiscard]] double sample(const PerlinNoise& noise, double x, double y, double z, double yScale,
                            double yMax) noexcept {
    const auto at = [&noise](std::int32_t index) noexcept -> std::uint8_t {
        return noise.permutation()[static_cast<std::size_t>(index & 0xFF)];
    };
    const double shiftedX = x + noise.originX();
    const double shiftedY = y + noise.originY();
    const double shiftedZ = z + noise.originZ();

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
        const double clamped = (yMax < localY) ? yMax : localY;
        localY -= std::floor((clamped / yScale) + 1.0e-7) * yScale;
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

[[nodiscard]] double sample(const PerlinNoise& noise, double x, double y, double z) noexcept {
    return sample(noise, x, y, z, 0.0, 0.0);
}

[[nodiscard]] double sampleSimplex2D(const PerlinNoise& noise, double x, double y) noexcept {
    const auto at = [&noise](std::int32_t index) noexcept -> std::uint8_t {
        return noise.permutation()[static_cast<std::size_t>(index & 0xFF)];
    };
    const double kSkew = 0.5 * (std::sqrt(3.0) - 1.0);
    const double kUnskew = (3.0 - std::sqrt(3.0)) / 6.0;

    const double skewed = (x + y) * kSkew;
    const auto cellX = static_cast<std::int32_t>(std::floor(x + skewed));
    const auto cellY = static_cast<std::int32_t>(std::floor(y + skewed));

    const double unskewed = static_cast<double>(cellX + cellY) * kUnskew;
    const double x0 = x - (static_cast<double>(cellX) - unskewed);
    const double y0 = y - (static_cast<double>(cellY) - unskewed);

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

struct Octave {
    PerlinNoise noise;
    double amplitude = 0.0;
    double persistence = 0.0;
    double frequency = 0.0;
};

using Octaves = std::vector<Octave>;

[[nodiscard]] Octaves createOctaves(Xoroshiro128PlusPlus& random, int firstOctave,
                                    std::span<const double> amplitudes) {
    Octaves octaves;
    double frequency = std::ldexp(1.0, firstOctave);
    const auto count = static_cast<int>(amplitudes.size());
    double persistence =
        count == 0 ? 0.0 : std::ldexp(1.0, count - 1) / (std::ldexp(1.0, count) - 1.0);
    const auto baseLo = static_cast<std::uint64_t>(random.nextLong());
    const auto baseHi = static_cast<std::uint64_t>(random.nextLong());
    for (std::size_t i = 0; i < amplitudes.size(); ++i) {
        if (!isZero(amplitudes[i])) {
            const std::string name = "octave_" + std::to_string(firstOctave + static_cast<int>(i));
            const stratum::rng::Seed128 salt = stratum::rng::seedFromHashOf(name);
            Xoroshiro128PlusPlus octaveRandom{
                stratum::rng::Seed128{.lo = baseLo ^ salt.lo, .hi = baseHi ^ salt.hi}};
            octaves.push_back(Octave{.noise = PerlinNoise::fromRandom(octaveRandom),
                                     .amplitude = amplitudes[i],
                                     .persistence = persistence,
                                     .frequency = frequency});
        }
        frequency *= 2.0;
        persistence *= 0.5;
    }
    return octaves;
}

[[nodiscard]] Octaves createLegacyOctaves(JavaRandom& random, int firstOctave, int octaveCount) {
    const int top = firstOctave + octaveCount - 1;
    constexpr int kStepsPerBlock = 262;
    for (int step = 0; step < -top * kStepsPerBlock; ++step) {
        static_cast<void>(random.nextInt());
    }
    Octaves octaves;
    double frequency = std::ldexp(1.0, top);
    double persistence = 1.0 / (std::ldexp(1.0, octaveCount) - 1.0);
    for (int octave = 0; octave < octaveCount; ++octave) {
        octaves.push_back(Octave{.noise = PerlinNoise::fromRandom(random),
                                 .amplitude = 1.0,
                                 .persistence = persistence,
                                 .frequency = frequency});
        frequency *= 0.5;
        persistence *= 2.0;
    }
    return octaves;
}

[[nodiscard]] double sampleOctaves(const Octaves& octaves, double x, double y, double z) noexcept {
    double total = 0.0;
    for (const Octave& octave : octaves) {
        total += (sample(octave.noise, x * octave.frequency, y * octave.frequency,
                         z * octave.frequency) *
                  octave.amplitude) *
                 octave.persistence;
    }
    return total;
}

constexpr double kSecondFrequency = 337.0 / 331.0;

struct Normal {
    Octaves first;
    Octaves second;
    double valueFactor = 1.0;
};

[[nodiscard]] Normal createNormal(Xoroshiro128PlusPlus& random, int firstOctave,
                                  std::span<const double> amplitudes) {
    Octaves firstStack = createOctaves(random, firstOctave, amplitudes);
    Octaves secondStack = createOctaves(random, firstOctave, amplitudes);
    Normal noise{std::move(firstStack), std::move(secondStack), 1.0};
    std::size_t first = 0;
    std::size_t last = amplitudes.size();
    while (last > first && isZero(amplitudes[last - 1])) {
        --last;
    }
    while (first < last && isZero(amplitudes[first])) {
        ++first;
    }
    const auto effective = static_cast<double>(last - first);
    noise.valueFactor = (1.0 / 6.0) / (0.1 * (1.0 + (1.0 / effective)));
    return noise;
}

[[nodiscard]] Normal createLegacyNormal(JavaRandom& random, int firstOctave, int octaveCount) {
    Octaves firstStack = createLegacyOctaves(random, firstOctave, octaveCount);
    Octaves secondStack = createLegacyOctaves(random, firstOctave, octaveCount);
    const auto length = static_cast<double>(octaveCount);
    return Normal{std::move(firstStack), std::move(secondStack),
                  (10.0 / 6.0) * length / (length + 1.0)};
}

[[nodiscard]] double sampleNormal(const Normal& noise, double x, double y, double z) noexcept {
    const double combined = sampleOctaves(noise.first, x, y, z) +
                            sampleOctaves(noise.second, x * kSecondFrequency, y * kSecondFrequency,
                                          z * kSecondFrequency);
    return combined * noise.valueFactor;
}

constexpr double kBaseFrequency = 684.412;
constexpr double kPrecisionPeriod = 33554432.0;

[[nodiscard]] double clampedLerp(double part, double from, double to) noexcept {
    if (!(part > 0.0)) {
        return from;
    }
    if (part >= 1.0) {
        return to;
    }
    return from + (part * (to - from));
}

[[nodiscard]] double maintainPrecision(double value) noexcept {
    return value - (std::floor((value / kPrecisionPeriod) + 0.5) * kPrecisionPeriod);
}

[[nodiscard]] double smearCap(BlendedNoise::Reading reading, double y, double slab,
                              double bareSlab) noexcept {
    if (reading == BlendedNoise::Reading::PreModern) {
        return y * slab;
    }
    if (y < 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return y * bareSlab;
}

struct Blended {
    std::vector<PerlinNoise> minimum_;
    std::vector<PerlinNoise> maximum_;
    std::vector<PerlinNoise> blend_;
    BlendedNoise::Parameters parameters_;
    BlendedNoise::Reading reading_ = BlendedNoise::Reading::PreModern;
};

template<typename Random>
[[nodiscard]] Blended buildBlended(Random& random, BlendedNoise::Parameters parameters,
                                   BlendedNoise::Reading reading) {
    Blended noise;
    noise.parameters_ = parameters;
    noise.reading_ = reading;
    for (std::size_t i = 0; i < BlendedNoise::kLimitOctaves; ++i) {
        noise.minimum_.push_back(PerlinNoise::fromRandom(random));
    }
    for (std::size_t i = 0; i < BlendedNoise::kLimitOctaves; ++i) {
        noise.maximum_.push_back(PerlinNoise::fromRandom(random));
    }
    for (std::size_t i = 0; i < BlendedNoise::kBlendOctaves; ++i) {
        noise.blend_.push_back(PerlinNoise::fromRandom(random));
    }
    return noise;
}

[[nodiscard]] double sampleBlended(const Blended& self, double x, double y, double z) noexcept {
    const BlendedNoise::Parameters& parameters = self.parameters_;
    const BlendedNoise::Reading reading = self.reading_;
    const double xzScale = kBaseFrequency * parameters.xzScale;
    const double yScale = kBaseFrequency * parameters.yScale;
    const double xzStep = xzScale / parameters.xzFactor;
    const double yStep = yScale / parameters.yFactor;

    double blend = 0.0;
    double persistence = 1.0;
    double contribution = 1.0;
    for (std::size_t i = 0; i < BlendedNoise::kBlendOctaves; ++i) {
        const double blendX = maintainPrecision(x * xzStep * persistence);
        const double blendY = maintainPrecision(y * yStep * persistence);
        const double blendZ = maintainPrecision(z * xzStep * persistence);
        const double blendBare = yStep * persistence;
        const double blendSmear = blendBare * parameters.smearScaleMultiplier;
        const double blendCap = smearCap(reading, y, blendSmear, blendBare);
        blend +=
            sample(self.blend_[i], blendX, blendY, blendZ, blendSmear, blendCap) * contribution;
        persistence *= 0.5;
        contribution *= 2.0;
    }
    const double part = 0.5 + (0.05 * blend);
    const bool readsMinimum = !(part >= 1.0);
    const bool readsMaximum = part > 0.0;

    double minimum = 0.0;
    double maximum = 0.0;
    persistence = 1.0;
    contribution = 1.0;
    for (std::size_t i = 0; i < BlendedNoise::kLimitOctaves; ++i) {
        const double limitX = maintainPrecision(x * xzScale * persistence);
        const double limitY = maintainPrecision(y * yScale * persistence);
        const double limitZ = maintainPrecision(z * xzScale * persistence);
        const double limitBare = yScale * persistence;
        const double limitSmear = limitBare * parameters.smearScaleMultiplier;
        const double limitCap = smearCap(reading, y, limitSmear, limitBare);

        if (readsMinimum) {
            minimum += sample(self.minimum_[i], limitX, limitY, limitZ, limitSmear, limitCap) *
                       contribution;
        }
        if (readsMaximum) {
            maximum += sample(self.maximum_[i], limitX, limitY, limitZ, limitSmear, limitCap) *
                       contribution;
        }

        persistence *= 0.5;
        contribution *= 2.0;
    }

    const double divisor = reading == BlendedNoise::Reading::PreModern ? 512.0 : 65536.0;
    return clampedLerp(part, minimum / divisor, maximum / divisor);
}

constexpr std::int32_t kCellRadius = 12;
constexpr std::int64_t kInnerVoidRadiusSquared = 4096;
constexpr double kIslandThreshold = -0.9;

[[nodiscard]] float clampHeight(float value) noexcept {
    return std::min(80.0F, std::max(-100.0F, value));
}

[[nodiscard]] float centralIslandHeight(const std::int32_t gridX,
                                        const std::int32_t gridZ) noexcept {
    const auto squared =
        (static_cast<std::int64_t>(gridX) * gridX) + (static_cast<std::int64_t>(gridZ) * gridZ);
    const auto distance = static_cast<float>(std::sqrt(static_cast<double>(squared)));
    return clampHeight(100.0F - (distance * 8.0F));
}

[[nodiscard]] float heightAt(const PerlinNoise& source_, const std::int32_t gridX,
                             const std::int32_t gridZ) noexcept {
    const std::int32_t cellX = stratum::javamath::floorDiv(gridX, 2);
    const std::int32_t cellZ = stratum::javamath::floorDiv(gridZ, 2);
    const std::int32_t withinX = stratum::javamath::floorMod(gridX, 2);
    const std::int32_t withinZ = stratum::javamath::floorMod(gridZ, 2);

    float height = centralIslandHeight(gridX, gridZ);

    for (std::int32_t offsetX = -kCellRadius; offsetX <= kCellRadius; ++offsetX) {
        for (std::int32_t offsetZ = -kCellRadius; offsetZ <= kCellRadius; ++offsetZ) {
            const auto islandX = static_cast<std::int64_t>(cellX) + offsetX;
            const auto islandZ = static_cast<std::int64_t>(cellZ) + offsetZ;
            if ((islandX * islandX) + (islandZ * islandZ) <= kInnerVoidRadiusSquared) {
                continue;
            }
            if (sampleSimplex2D(source_, static_cast<double>(islandX),
                                static_cast<double>(islandZ)) >= kIslandThreshold) {
                continue;
            }
            const auto steepness = static_cast<float>(
                (((std::abs(islandX) * 3439) + (std::abs(islandZ) * 147)) % 13) + 9);
            const auto localX = static_cast<float>(withinX - (offsetX * 2));
            const auto localZ = static_cast<float>(withinZ - (offsetZ * 2));
            const auto distance = static_cast<float>(
                std::sqrt(static_cast<double>((localX * localX) + (localZ * localZ))));
            height = std::max(height, clampHeight(100.0F - (distance * steepness)));
        }
    }
    return height;
}

[[nodiscard]] double endSample(const PerlinNoise& source, const std::int32_t blockX,
                               const std::int32_t blockZ) noexcept {
    const float height = heightAt(source, stratum::javamath::floorDiv(blockX, 8),
                                  stratum::javamath::floorDiv(blockZ, 8));
    return (static_cast<double>(height) - 8.0) / 128.0;
}

} // namespace base

// ---------------------------------------------------------------------------
// Inputs.
// ---------------------------------------------------------------------------

/// Draws test inputs from the project's own generator, so that a failure
/// reproduces from its seed on every platform.
class Inputs {
public:
    explicit Inputs(std::int64_t seed) : random_(seed) {}

    [[nodiscard]] double unit() noexcept { return random_.nextDouble(); }

    [[nodiscard]] std::int32_t below(std::int32_t bound) noexcept { return random_.nextInt(bound); }

    /// A uniform double in [-limit, limit].
    [[nodiscard]] double uniform(double limit) noexcept { return ((unit() * 2.0) - 1.0) * limit; }

    /// A magnitude class, from below one to the int32 lattice limit, with
    /// 2^24 and 2^25 (maintainPrecision's fold) bracketed on both sides.
    [[nodiscard]] double magnitude(double ceiling) noexcept {
        static constexpr std::array<double, 16> kMagnitudes = {
            1.0,        2.0,        16.0,         256.0,       4096.0,     65536.0,
            1048576.0,  16777215.0, 16777216.0,   16777217.0,  33554431.0, 33554432.0,
            33554433.0, 67108864.0, 1073741824.0, 2147483136.0};
        return std::min(ceiling, kMagnitudes[static_cast<std::size_t>(below(16))]);
    }

    /// A coordinate whose magnitude stays within @p ceiling, drawn from one
    /// of several shapes. Every shape lands on both signs.
    [[nodiscard]] double coordinate(double ceiling) noexcept {
        const double limit = magnitude(ceiling);
        switch (below(9)) {
            case 0:
            case 1:
                return uniform(limit);
            case 2: // an exact integer
                return std::floor(uniform(limit - 1.0));
            case 3: // a half-integer
                return std::floor(uniform(limit - 1.0)) + 0.5;
            case 4: { // an integer, one ulp to either side
                const double integer = std::floor(uniform(limit - 1.0));
                return std::nextafter(integer, below(2) == 0 ? -kInfinity : kInfinity);
            }
            case 5: // near the origin, where the cell is 0 or -1
                return uniform(2.0);
            case 6:
                return special();
            case 7: { // a dyadic fraction, exact at every magnitude it fits
                const double integer = std::floor(uniform(limit - 1.0));
                return integer + (static_cast<double>(below(16)) / 16.0);
            }
            default: // a double next to the magnitude, never past the ceiling
                return std::nextafter(limit,
                                      (below(2) == 0 || limit >= ceiling) ? 0.0 : kInfinity) *
                       (below(2) == 0 ? -1.0 : 1.0);
        }
    }

    /// The values a floor or a sign test is most likely to get wrong.
    [[nodiscard]] double special() noexcept {
        static constexpr std::array<double, 14> kSpecials = {
            0.0,
            -0.0,
            std::numeric_limits<double>::denorm_min(),
            -std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::min(),
            -std::numeric_limits<double>::min(),
            1.0,
            -1.0,
            0.5,
            -0.5,
            0.99999999999999989, // the double below one
            -0.99999999999999989,
            1.0000000000000002, // the double above one
            -1.0000000000000002};
        return kSpecials[static_cast<std::size_t>(below(14))];
    }

    /// A coordinate that puts `coordinate + origin` in a cell whose low byte
    /// is 0, 1, 254 or 255, on either side of zero — the permutation table's
    /// wraps — at the start, the middle, or the last double of the cell.
    [[nodiscard]] double latticeEdge(double origin) noexcept {
        static constexpr std::array<double, 4> kBytes = {0.0, 1.0, 254.0, 255.0};
        const double block = std::floor(uniform(8388606.0)); // |block * 256| < 2^31 - 512
        const double cell = (block * 256.0) + kBytes[static_cast<std::size_t>(below(4))];
        double within = 0.0;
        switch (below(4)) {
            case 0:
                within = 0.0;
                break;
            case 1:
                within = 0.5;
                break;
            case 2:
                within = unit();
                break;
            default:
                within = 0.99999999999999989;
                break;
        }
        return (cell + within) - origin;
    }

    /// A fold scale: the slab widths BlendedNoise passes, and the edges.
    [[nodiscard]] double yScale() noexcept {
        switch (below(6)) {
            case 0:
                return unit() * 2048.0;
            case 1: // a vanilla slab: 684.412 * yScale / yFactor (or not), halved per octave
                return (684.412 * (below(2) == 0 ? 0.125 : 1.0) / (below(2) == 0 ? 160.0 : 1.0)) *
                       std::ldexp(1.0, -below(16)) * (below(2) == 0 ? 8.0 : 4.0);
            case 2:
                return unit();
            case 3: {
                static constexpr std::array<double, 10> kEdges = {
                    0.0,
                    -0.0,
                    std::numeric_limits<double>::denorm_min(),
                    1.0e-300,
                    std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN(),
                    1.0,
                    1.0e300,
                    0.5};
                return kEdges[static_cast<std::size_t>(below(10))];
            }
            case 4:
                return -unit() * 64.0;
            default:
                return std::ldexp(1.0, below(40) - 20);
        }
    }

    /// A fold cap: what smearCap hands over, and the edges.
    [[nodiscard]] double yMax(double yScale) noexcept {
        switch (below(6)) {
            case 0:
                return uniform(2.0);
            case 1:
                return uniform(4096.0) * yScale;
            case 2:
                return std::floor(uniform(8.0)) * yScale; // an exact multiple of the slab
            case 3: {
                static constexpr std::array<double, 6> kEdges = {
                    std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN(),
                    0.0,
                    -0.0,
                    1.0e-7};
                return kEdges[static_cast<std::size_t>(below(6))];
            }
            case 4:
                return unit();
            default:
                return uniform(1.0e6);
        }
    }

private:
    Xoroshiro128PlusPlus random_;
};

/// The Perlin octaves the comparisons run on: both generators' constructions
/// at several seeds, and hand-built ones at the origin's edges — a -0.0
/// origin (so that `coordinate + origin` can be -0.0), a zero one, one a ulp
/// below 256, and a reversed identity permutation.
[[nodiscard]] std::vector<PerlinNoise> perlinSubjects() {
    std::vector<PerlinNoise> subjects;
    for (const std::int64_t seed : {0LL, 1LL, 42LL, -7LL, 123456789LL}) {
        Xoroshiro128PlusPlus modern{seed};
        subjects.push_back(PerlinNoise::fromRandom(modern));
        JavaRandom legacy{seed};
        subjects.push_back(PerlinNoise::fromRandom(legacy));
    }
    std::array<std::uint8_t, 256> identity{};
    std::array<std::uint8_t, 256> reversed{};
    for (std::size_t i = 0; i < identity.size(); ++i) {
        identity[i] = static_cast<std::uint8_t>(i);
        reversed[i] = static_cast<std::uint8_t>(255U - i);
    }
    subjects.emplace_back(-0.0, -0.0, -0.0, identity);
    subjects.emplace_back(0.0, 0.0, 0.0, reversed);
    subjects.emplace_back(255.99999999999997, 255.99999999999997, 255.99999999999997, identity);
    subjects.emplace_back(0.5, 127.25, 255.5, subjects.front().permutation());
    return subjects;
}

/// The highest |coordinate| a Perlin sample may take and keep its lattice
/// cell in int32 whatever the origin in [0, 256).
constexpr double kPerlinReach = 2147483136.0;

} // namespace

TEST_CASE("perlin sampling is the previous implementation's, to the bit",
          "[noise][perlin][differential]") {
    const std::vector<PerlinNoise> subjects = perlinSubjects();
    Inputs inputs{20261029};
    std::size_t mismatches = 0;
    std::size_t compared = 0;
    for (int i = 0; i < 400000; ++i) {
        const PerlinNoise& noise = subjects[static_cast<std::size_t>(
            inputs.below(static_cast<std::int32_t>(subjects.size())))];
        // One axis in four, take a lattice edge; the rest a coordinate.
        const auto axis = [&](double origin) {
            return inputs.below(4) == 0 ? inputs.latticeEdge(origin)
                                        : inputs.coordinate(kPerlinReach);
        };
        const double x = axis(noise.originX());
        const double y = axis(noise.originY());
        const double z = axis(noise.originZ());

        // The three-argument form, and the five-argument form unfolded at
        // both zeros, which must be the same double.
        const double plain = noise.sample(x, y, z);
        const double expected = base::sample(noise, x, y, z);
        mismatches += bits(plain) != bits(expected) ? 1U : 0U;
        mismatches += bits(noise.sample(x, y, z, 0.0, 0.0)) != bits(expected) ? 1U : 0U;
        mismatches +=
            bits(noise.sample(x, y, z, -0.0, inputs.yMax(1.0))) != bits(expected) ? 1U : 0U;

        // The folded form, at a scale and cap from the edges.
        const double yScale = inputs.yScale();
        const double yMax = inputs.yMax(yScale);
        const double folded = noise.sample(x, y, z, yScale, yMax);
        const double foldedExpected = base::sample(noise, x, y, z, yScale, yMax);
        if (bits(folded) != bits(foldedExpected)) {
            ++mismatches;
            UNSCOPED_INFO("folded at " << x << ", " << y << ", " << z << " scale " << yScale
                                       << " cap " << yMax);
        }
        compared += 4;
    }
    INFO(compared << " samples compared");
    CHECK(mismatches == 0U);
}

TEST_CASE("perlin sampling agrees at signed zeros and the origin's own edges",
          "[noise][perlin][differential]") {
    // Exhaustive over a small grid of the values most likely to separate a
    // floor rewrite from std::floor: each axis independently.
    const std::vector<PerlinNoise> subjects = perlinSubjects();
    const std::array<double, 12> values = {0.0,
                                           -0.0,
                                           0.5,
                                           -0.5,
                                           1.0,
                                           -1.0,
                                           255.0,
                                           -255.0,
                                           256.0,
                                           -256.0,
                                           std::numeric_limits<double>::denorm_min(),
                                           -std::numeric_limits<double>::denorm_min()};
    std::size_t mismatches = 0;
    for (const PerlinNoise& noise : subjects) {
        for (const double x : values) {
            for (const double y : values) {
                for (const double z : values) {
                    mismatches +=
                        bits(noise.sample(x, y, z)) != bits(base::sample(noise, x, y, z)) ? 1U : 0U;
                    // And with the origin taken back out, so that the shifted
                    // coordinate itself is the edge value.
                    const double sx = x - noise.originX();
                    const double sy = y - noise.originY();
                    const double sz = z - noise.originZ();
                    mismatches +=
                        bits(noise.sample(sx, sy, sz)) != bits(base::sample(noise, sx, sy, sz))
                            ? 1U
                            : 0U;
                    mismatches += bits(noise.sample(sx, sy, sz, 0.75, sy)) !=
                                          bits(base::sample(noise, sx, sy, sz, 0.75, sy))
                                      ? 1U
                                      : 0U;
                }
            }
        }
    }
    CHECK(mismatches == 0U);
}

TEST_CASE("simplex sampling is the previous implementation's, to the bit",
          "[noise][simplex][differential]") {
    const std::vector<PerlinNoise> subjects = perlinSubjects();
    Inputs inputs{-31};
    std::size_t mismatches = 0;
    constexpr double kSimplexReach = 536870911.0; // below 2^29, sampleSimplex2D's bound
    for (int i = 0; i < 400000; ++i) {
        const PerlinNoise& noise = subjects[static_cast<std::size_t>(
            inputs.below(static_cast<std::int32_t>(subjects.size())))];
        double x = inputs.coordinate(kSimplexReach);
        double y = inputs.coordinate(kSimplexReach);
        if (inputs.below(3) == 0) { // the End's own inputs: integer cells
            x = std::floor(x);
            y = std::floor(y);
        }
        mismatches +=
            bits(noise.sampleSimplex2D(x, y)) != bits(base::sampleSimplex2D(noise, x, y)) ? 1U : 0U;
    }
    CHECK(mismatches == 0U);
}

TEST_CASE("octave and normal noise are the previous implementation's, to the bit",
          "[noise][octave][normal][differential]") {
    // Amplitude lists shaped like vanilla's: sparse, non-power-of-two, with
    // leading and trailing zeros, one octave and many.
    const std::vector<std::pair<int, std::vector<double>>> shapes = {
        {-7, {1.0, 1.0}},
        {-3, {1.0}},
        {-9, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0}},
        {-10, {1.0, 1.25, 1.0}},
        {-8, {0.5, 1.0, 2.0, 1.0, 2.0, 1.0, 0.0, 2.0, 0.0}},
        {-16, {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0}},
        {-1, {0.0, 0.0, 1.0}},
        {0, {1.0, 0.5}},
        {2, {1.0}},
        {-5, {}},
    };
    Inputs inputs{987654321};
    std::size_t mismatches = 0;
    std::size_t compared = 0;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        const auto& [firstOctave, amplitudes] = shapes[s];
        for (const std::int64_t seed : {3LL, -1LL, 42LL}) {
            Xoroshiro128PlusPlus ours{seed + static_cast<std::int64_t>(s)};
            Xoroshiro128PlusPlus theirs{seed + static_cast<std::int64_t>(s)};
            const NormalNoise normal = NormalNoise::create(ours, firstOctave, amplitudes);
            const base::Normal expected = base::createNormal(theirs, firstOctave, amplitudes);
            Xoroshiro128PlusPlus oursOctave{seed * 31};
            Xoroshiro128PlusPlus theirsOctave{seed * 31};
            const OctaveNoise octave = OctaveNoise::create(oursOctave, firstOctave, amplitudes);
            const base::Octaves expectedOctave =
                base::createOctaves(theirsOctave, firstOctave, amplitudes);
            const double reach = std::min(normal.inputLimit(), kPerlinReach);
            for (int i = 0; i < 6000; ++i) {
                const double x = inputs.coordinate(reach);
                const double y = inputs.coordinate(reach);
                const double z = inputs.coordinate(reach);
                mismatches +=
                    bits(normal.sample(x, y, z)) != bits(base::sampleNormal(expected, x, y, z))
                        ? 1U
                        : 0U;
                mismatches += bits(octave.sample(x, y, z)) !=
                                      bits(base::sampleOctaves(expectedOctave, x, y, z))
                                  ? 1U
                                  : 0U;
                compared += 2;
            }
        }
    }
    // The legacy construction, highest frequency first.
    for (const auto& [firstOctave, count] :
         std::vector<std::pair<int, int>>{{-7, 2}, {-3, 1}, {-16, 16}, {-8, 4}}) {
        JavaRandom ours{static_cast<std::int64_t>(firstOctave) * 1000};
        JavaRandom theirs{static_cast<std::int64_t>(firstOctave) * 1000};
        const NormalNoise normal = NormalNoise::createLegacy(ours, firstOctave, count);
        const base::Normal expected = base::createLegacyNormal(theirs, firstOctave, count);
        const double reach = std::min(normal.inputLimit(), kPerlinReach);
        for (int i = 0; i < 6000; ++i) {
            const double x = inputs.coordinate(reach);
            const double y = inputs.coordinate(reach);
            const double z = inputs.coordinate(reach);
            mismatches +=
                bits(normal.sample(x, y, z)) != bits(base::sampleNormal(expected, x, y, z)) ? 1U
                                                                                            : 0U;
            ++compared;
        }
    }
    CHECK(bits(NormalNoise::zero().sample(1.0, 2.0, 3.0)) == bits(0.0));
    INFO(compared << " samples compared");
    CHECK(mismatches == 0U);
}

TEST_CASE("maintainPrecision is the previous implementation's, to the bit",
          "[noise][blended][differential]") {
    Inputs inputs{55};
    std::size_t mismatches = 0;
    const auto check = [&mismatches](double value) {
        if (bits(stratum::noise::maintainPrecision(value)) !=
            bits(base::maintainPrecision(value))) {
            ++mismatches;
            UNSCOPED_INFO("maintainPrecision(" << value << ")");
        }
    };
    for (int i = 0; i < 400000; ++i) {
        const double scale = std::ldexp(1.0, inputs.below(80) - 10);
        check(inputs.uniform(1.0) * scale);
        check(inputs.coordinate(2147483136.0) * (inputs.below(2) == 0 ? 1.0 : 684.412));
        // On and either side of a half period: where the half-up rounding
        // and the fold's edge live.
        const double halves = std::floor(inputs.uniform(1.0e6)) + 0.5;
        const double edge = halves * 33554432.0;
        check(edge);
        check(std::nextafter(edge, kInfinity));
        check(std::nextafter(edge, -kInfinity));
    }
    for (const double value : {0.0,
                               -0.0,
                               16777216.0,
                               -16777216.0,
                               16777216.0 - 0x1p-29,
                               -16777216.0 + 0x1p-29,
                               33554432.0,
                               -33554432.0,
                               0x1p52,
                               -0x1p52,
                               0x1p53,
                               -0x1p53,
                               0x1p60,
                               0x1p77,
                               0x1p100,
                               1.0e300,
                               -1.0e300,
                               std::numeric_limits<double>::max(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::denorm_min(),
                               -std::numeric_limits<double>::denorm_min()}) {
        check(value);
        check(std::nextafter(value, kInfinity));
        check(std::nextafter(value, -kInfinity));
    }
    CHECK(mismatches == 0U);
}

TEST_CASE("blended noise is the previous implementation's, to the bit",
          "[noise][blended][differential]") {
    // Vanilla's two parameter sets, a multiplier of one (where the readings
    // meet), a zero multiplier (no fold at all), and an off-vanilla one.
    const std::vector<BlendedNoise::Parameters> parameterSets = {
        {.xzScale = 0.25,
         .yScale = 0.125,
         .xzFactor = 80.0,
         .yFactor = 160.0,
         .smearScaleMultiplier = 8.0},
        {.xzScale = 2.0,
         .yScale = 1.0,
         .xzFactor = 80.0,
         .yFactor = 160.0,
         .smearScaleMultiplier = 4.0},
        {.xzScale = 1.0,
         .yScale = 1.0,
         .xzFactor = 80.0,
         .yFactor = 160.0,
         .smearScaleMultiplier = 1.0},
        {.xzScale = 0.25,
         .yScale = 0.125,
         .xzFactor = 80.0,
         .yFactor = 160.0,
         .smearScaleMultiplier = 0.0},
        {.xzScale = 3.5,
         .yScale = 0.001,
         .xzFactor = 7.0,
         .yFactor = 0.5,
         .smearScaleMultiplier = 0.3},
    };
    Inputs inputs{-20261029};
    std::size_t mismatches = 0;
    std::size_t compared = 0;
    for (const BlendedNoise::Parameters& parameters : parameterSets) {
        for (const std::int64_t seed : {42LL, -1LL}) {
            stratum::rng::Xoroshiro128PlusPlus modernRandom =
                stratum::rng::XoroshiroPositionalFactory(seed).fromHashOf("minecraft:terrain");
            JavaRandom legacyRandom{seed};
            JavaRandom preModernRandom{seed + 1};
            JavaRandom leverRandom{seed + 2};
            const std::array<std::pair<BlendedNoise, base::Blended>, 4> pairs = {
                std::pair{
                    BlendedNoise::modern(seed, parameters),
                    base::buildBlended(modernRandom, parameters, BlendedNoise::Reading::Modern)},
                std::pair{
                    BlendedNoise::legacyFromWorldSeed(seed, parameters),
                    base::buildBlended(legacyRandom, parameters, BlendedNoise::Reading::Modern)},
                std::pair{[&] {
                              JavaRandom random{seed + 1};
                              return BlendedNoise::legacy(random, parameters);
                          }(),
                          base::buildBlended(preModernRandom, parameters,
                                             BlendedNoise::Reading::PreModern)},
                std::pair{
                    [&] {
                        JavaRandom random{seed + 2};
                        return BlendedNoise::withModernReading(random, parameters);
                    }(),
                    base::buildBlended(leverRandom, parameters, BlendedNoise::Reading::Modern)},
            };
            for (const auto& [ours, theirs] : pairs) {
                for (int i = 0; i < 1500; ++i) {
                    // Block coordinates, mostly: the world's own range in x
                    // and z, the build height and beyond in y; and now and
                    // then anything at all, for the wrap.
                    double x = std::floor(inputs.uniform(30000000.0));
                    double y = std::floor(inputs.uniform(2048.0));
                    double z = std::floor(inputs.uniform(30000000.0));
                    if (inputs.below(4) == 0) {
                        x = inputs.coordinate(2147483136.0);
                        y = inputs.coordinate(4096.0);
                        z = inputs.coordinate(2147483136.0);
                    }
                    if (bits(ours.sample(x, y, z)) != bits(base::sampleBlended(theirs, x, y, z))) {
                        ++mismatches;
                        UNSCOPED_INFO("blended at " << x << ", " << y << ", " << z);
                    }
                    ++compared;
                }
            }
        }
    }
    INFO(compared << " samples compared");
    CHECK(mismatches == 0U);
}

TEST_CASE("the End's island field is the previous implementation's, to the bit",
          "[noise][end_islands][differential]") {
    Inputs inputs{17292};
    std::size_t mismatches = 0;
    std::size_t raised = 0;
    std::size_t compared = 0;
    for (const std::int64_t seed : {0LL, 42LL, -9LL}) {
        const EndIslands islands = EndIslands::fromWorldSeed(seed);
        JavaRandom random{seed};
        for (int i = 0; i < 17292; ++i) {
            static_cast<void>(random.nextInt());
        }
        const PerlinNoise source = PerlinNoise::fromRandom(random);
        const auto compare = [&](std::int32_t gridX, std::int32_t gridZ) {
            const float ours = islands.heightAt(gridX, gridZ);
            const float theirs = base::heightAt(source, gridX, gridZ);
            if (bits(ours) != bits(theirs)) {
                ++mismatches;
                UNSCOPED_INFO("heightAt(" << gridX << ", " << gridZ << ")");
            }
            // Columns an outer island lifts: what the field exists for.
            raised += theirs > base::centralIslandHeight(gridX, gridZ) ? 1U : 0U;
            ++compared;
        };
        // A strip through the inner void's edge and the central island, the
        // outer field around it, and anywhere in the grid's range.
        for (std::int32_t gridX = -300; gridX <= 300; gridX += 3) {
            compare(gridX, 2 * gridX / 3);
        }
        for (int i = 0; i < 400; ++i) {
            compare(inputs.below(20000) - 10000, inputs.below(20000) - 10000);
            const std::int32_t far = (1 << 28) - inputs.below(1 << 20);
            compare(inputs.below(2) == 0 ? far : -far, inputs.below(1 << 29) - (1 << 28));
        }
        for (int i = 0; i < 100; ++i) {
            const auto blockX =
                static_cast<std::int32_t>(static_cast<std::uint32_t>(inputs.below(1 << 30)) * 4U);
            const auto blockZ = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(inputs.below(1 << 30)) * 4U + 3U);
            mismatches += bits(islands.sample(blockX, blockZ)) !=
                                  bits(base::endSample(source, blockX, blockZ))
                              ? 1U
                              : 0U;
        }
        for (const std::int32_t edge : {std::numeric_limits<std::int32_t>::min(),
                                        std::numeric_limits<std::int32_t>::max(), 0, -1, 1}) {
            mismatches +=
                bits(islands.sample(edge, edge)) != bits(base::endSample(source, edge, edge)) ? 1U
                                                                                              : 0U;
            mismatches += bits(islands.sample(edge, -1 - edge)) !=
                                  bits(base::endSample(source, edge, -1 - edge))
                              ? 1U
                              : 0U;
        }
    }
    INFO(compared << " columns compared, " << raised << " of them lifted by an outer island");
    CHECK(mismatches == 0U);
    // The comparison means something only if the outer term is exercised.
    CHECK(raised * 10U > compared);
}
