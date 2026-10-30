// Stratum — a column of noise samples at once, held to the scalar sample bit for bit.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// NormalNoise::sampleColumn and BlendedNoise::sampleColumn sample up to
// kColumn points that share x and z, making once what the points share — the
// call, and in every octave the x and z axes of the lattice (and for the
// blended noise each octave's wrapped x and z). Each lane must be exactly the
// double sample() returns at its point. This compares the two by their bytes
// over seeded noises of every construction and points of every kind: either
// sign, near the origin and far out (where the blended noise's coordinates
// wrap), fractions on and off the lattice, y both sides of 0 (the blended
// noise's cap does not bind below it), whole cell columns as the interpreter
// asks for them and arbitrary ones, duplicates, every count from 1 to
// kColumn. The "[.noise-column-long]" case runs millions of points.

#include <stratum/noise/blended.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using stratum::noise::BlendedNoise;
using stratum::noise::NormalNoise;
using stratum::rng::JavaRandom;
using stratum::rng::Xoroshiro128PlusPlus;

[[nodiscard]] std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

/// Draws, from the project's own generator.
class Draw {
public:
    explicit Draw(std::int64_t seed) : random_(seed) {}

    [[nodiscard]] std::int32_t below(std::int32_t bound) { return random_.nextInt(bound); }

    [[nodiscard]] double unit() { return random_.nextDouble(); }

    [[nodiscard]] std::int64_t seed() { return random_.nextLong(); }

    /// A coordinate of some kind: a whole block, a fraction, one a hair
    /// either side of a whole number, near 0, or far out; either sign.
    [[nodiscard]] double coordinate(double reach) {
        const double sign = below(2) == 0 ? -1.0 : 1.0;
        switch (below(6)) {
            case 0:
                return sign * static_cast<double>(below(1 << 20));
            case 1:
                return sign * unit() * 16.0;
            case 2: {
                const auto whole = static_cast<double>(below(1 << 16));
                return sign *
                       (below(2) == 0 ? std::nextafter(whole, 0.0) : std::nextafter(whole, 1.0e9));
            }
            case 3:
                return sign * unit() * reach;
            case 4:
                return sign * (static_cast<double>(below(1 << 24)) + 0.5);
            default:
                return sign * unit() * 1000.0;
        }
    }

private:
    Xoroshiro128PlusPlus random_;
};

/// A column of at most @p most ys: a cell column's corners, scaled as a
/// `noise` node scales them, or anything at all; as often a few as many.
[[nodiscard]] std::vector<double> column(Draw& draw, double reach, std::size_t most) {
    const auto count = static_cast<std::size_t>(
        draw.below(2) == 0 ? draw.below(8) + 1 : draw.below(static_cast<std::int32_t>(most)) + 1);
    std::vector<double> ys(count);
    if (draw.below(2) == 0) {
        const double step = draw.below(2) == 0 ? 8.0 : 4.0;
        const double scale =
            std::array<double, 4>{1.0, 0.5, 0.25, 0.0}[static_cast<std::size_t>(draw.below(4))];
        const auto first = static_cast<double>(draw.below(512) - 256);
        for (std::size_t k = 0; k < count; ++k) {
            ys[k] = (first + (step * static_cast<double>(k))) * scale;
        }
    } else {
        for (double& y : ys) {
            y = draw.coordinate(reach);
        }
        if (count > 1 && draw.below(4) == 0) {
            ys[count - 1] = ys[0];
        }
    }
    return ys;
}

/// A normal noise of some construction.
[[nodiscard]] NormalNoise normalNoise(Draw& draw) {
    switch (draw.below(5)) {
        case 0: {
            JavaRandom random{draw.seed()};
            const int octaves = draw.below(8) + 1;
            return NormalNoise::createLegacy(random, -octaves - draw.below(4), octaves);
        }
        case 1:
            return NormalNoise::zero();
        default: {
            Xoroshiro128PlusPlus random{draw.seed()};
            std::vector<double> amplitudes(static_cast<std::size_t>(draw.below(10) + 1));
            for (double& amplitude : amplitudes) {
                amplitude = std::array<double, 6>{
                    0.0, 1.0, 2.0, 0.5, 1.5, -1.0}[static_cast<std::size_t>(draw.below(6))];
            }
            return NormalNoise::create(random, draw.below(14) - 11, amplitudes);
        }
    }
}

/// A blended noise of some construction and parameters.
[[nodiscard]] BlendedNoise blendedNoise(Draw& draw) {
    const BlendedNoise::Parameters parameters{
        .xzScale =
            std::array<double, 4>{0.25, 1.0, 0.003, 2.0}[static_cast<std::size_t>(draw.below(4))],
        .yScale =
            std::array<double, 4>{0.125, 0.25, 1.0, 0.0}[static_cast<std::size_t>(draw.below(4))],
        .xzFactor =
            std::array<double, 3>{80.0, 40.0, 13.5}[static_cast<std::size_t>(draw.below(3))],
        .yFactor =
            std::array<double, 3>{160.0, 60.0, 90.25}[static_cast<std::size_t>(draw.below(3))],
        .smearScaleMultiplier =
            std::array<double, 4>{8.0, 4.0, 1.0, 0.0}[static_cast<std::size_t>(draw.below(4))],
    };
    switch (draw.below(4)) {
        case 0: {
            JavaRandom random{draw.seed()};
            return BlendedNoise::legacy(random, parameters);
        }
        case 1: {
            JavaRandom random{draw.seed()};
            return BlendedNoise::withModernReading(random, parameters);
        }
        case 2:
            return BlendedNoise::legacyFromWorldSeed(draw.seed(), parameters);
        default:
            return BlendedNoise::modern(draw.seed(), parameters);
    }
}

struct Tally {
    std::size_t points = 0;
    std::size_t differing = 0;
    std::string first = "none";
};

template<typename Noise, typename Accepts>
void compareColumns(Draw& draw, const Noise& noise, std::size_t columns, double reach,
                    const Accepts& accepts, Tally& tally) {
    std::array<double, Noise::kColumn> out{};
    for (std::size_t c = 0; c < columns; ++c) {
        const double x = draw.coordinate(reach);
        const double z = draw.coordinate(reach);
        std::vector<double> ys = column(draw, reach, Noise::kColumn);
        // Only points the noise is defined at (NormalNoise::accepts).
        std::size_t count = 0;
        while (count < ys.size() && accepts(x, ys[count], z)) {
            ++count;
        }
        if (count == 0) {
            continue;
        }
        const std::span<const double> lanes(ys.data(), count);
        noise.sampleColumn(x, lanes, z, std::span<double>(out.data(), count));
        for (std::size_t k = 0; k < count; ++k) {
            ++tally.points;
            const double scalar = noise.sample(x, ys[k], z);
            if (bits(out[k]) != bits(scalar)) {
                if (tally.differing++ == 0) {
                    tally.first = "x " + std::to_string(x) + " y " + std::to_string(ys[k]) + " z " +
                                  std::to_string(z) + " lane " + std::to_string(k) + " of " +
                                  std::to_string(count);
                }
            }
        }
    }
}

void fuzzNormal(std::int64_t seed, std::size_t noises, std::size_t columns, Tally& tally) {
    Draw draw(seed);
    for (std::size_t n = 0; n < noises; ++n) {
        const NormalNoise noise = normalNoise(draw);
        // Out to the noise's own limit, and short of it.
        const double reach = std::isfinite(noise.inputLimit()) ? noise.inputLimit() : 1.0e9;
        compareColumns(
            draw, noise, columns, draw.below(3) == 0 ? reach : std::fmin(reach, 3.0e5),
            [&noise](double x, double y, double z) { return noise.accepts(x, y, z); }, tally);
    }
}

void fuzzBlended(std::int64_t seed, std::size_t noises, std::size_t columns, Tally& tally) {
    Draw draw(seed);
    for (std::size_t n = 0; n < noises; ++n) {
        const BlendedNoise noise = blendedNoise(draw);
        // World-border distances, where the coordinates wrap, and near ones.
        compareColumns(
            draw, noise, columns, draw.below(2) == 0 ? 3.0e7 : 2.0e4,
            [](double, double, double) { return true; }, tally);
    }
}

} // namespace

TEST_CASE("a normal noise's column is its samples, bit for bit", "[noise][column]") {
    Tally tally;
    fuzzNormal(20261030, 500, 120, tally);
    CAPTURE(tally.points, tally.first);
    CHECK(tally.differing == 0U);
    CHECK(tally.points > 800000U);
}

TEST_CASE("a normal noise at a y of zero is what its column makes there, bit for bit",
          "[noise][column]") {
    // sample() at y = +0.0 or -0.0 reads each octave's lattice along y off
    // its noise (PerlinNoise::flatYKnown) rather than making it; sampleColumn
    // makes it from the point's y as samplePerlin always did. The two must
    // agree at every point, whichever zero, for noises of every
    // construction, out to their input limits.
    Draw draw(4321);
    std::size_t points = 0;
    std::size_t differing = 0;
    std::string first = "none";
    std::array<double, 1> out{};
    for (std::size_t n = 0; n < 400; ++n) {
        const NormalNoise noise = normalNoise(draw);
        const double reach = std::isfinite(noise.inputLimit()) ? noise.inputLimit() : 1.0e9;
        for (std::size_t c = 0; c < 200; ++c) {
            const double x = draw.coordinate(draw.below(3) == 0 ? reach : std::fmin(reach, 3.0e5));
            const double z = draw.coordinate(draw.below(3) == 0 ? reach : std::fmin(reach, 3.0e5));
            const double y = draw.below(2) == 0 ? 0.0 : -0.0;
            if (!noise.accepts(x, y, z)) {
                continue;
            }
            const std::array<double, 1> ys{y};
            noise.sampleColumn(x, ys, z, out);
            ++points;
            if (bits(noise.sample(x, y, z)) != bits(out[0]) && differing++ == 0) {
                first = "x " + std::to_string(x) + " y " + std::to_string(y) + " z " +
                        std::to_string(z);
            }
        }
    }
    CAPTURE(points, first);
    CHECK(differing == 0U);
    CHECK(points > 50000U);
}

TEST_CASE("a Perlin octave knows its lattice along y at y = 0 only inside the lattice's range",
          "[noise][column]") {
    // flatYKnown is what lets sample() skip making it; an origin the lattice
    // cannot hold (out of int32, NaN) leaves it unknown.
    std::array<std::uint8_t, 256> identity{};
    for (std::size_t i = 0; i < identity.size(); ++i) {
        identity[i] = static_cast<std::uint8_t>(i);
    }
    using stratum::noise::PerlinNoise;
    CHECK(PerlinNoise(0.0, 17.25, 0.0, identity).flatYKnown());
    CHECK(PerlinNoise(0.0, 17.25, 0.0, identity).flatCellY() == 17U);
    CHECK(bits(PerlinNoise(0.0, 17.25, 0.0, identity).flatLocalY()) == bits(0.25));
    CHECK(PerlinNoise(0.0, -0.0, 0.0, identity).flatYKnown());
    CHECK(bits(PerlinNoise(0.0, -0.0, 0.0, identity).flatLocalY()) == bits(0.0));
    CHECK_FALSE(PerlinNoise(0.0, 3.0e9, 0.0, identity).flatYKnown());
    CHECK_FALSE(PerlinNoise(0.0, std::nan(""), 0.0, identity).flatYKnown());
}

TEST_CASE("a blended noise's column is its samples, bit for bit", "[noise][column]") {
    Tally tally;
    fuzzBlended(7171, 80, 100, tally);
    CAPTURE(tally.points, tally.first);
    CHECK(tally.differing == 0U);
    CHECK(tally.points > 100000U);
}

TEST_CASE("noise columns are their samples over millions of points (long)",
          "[.noise-column-long]") {
    Tally normal;
    fuzzNormal(91, 20000, 120, normal);
    Tally blended;
    fuzzBlended(92, 4000, 100, blended);
    CAPTURE(normal.points, normal.first, blended.points, blended.first);
    CHECK(normal.differing == 0U);
    CHECK(blended.differing == 0U);
    CHECK(normal.points > 5000000U);
    CHECK(blended.points > 1000000U);
}
