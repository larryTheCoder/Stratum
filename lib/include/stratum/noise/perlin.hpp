// Stratum — Perlin, octave, normal and simplex noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The noise vanilla's density functions are built from. Implemented from the
// documented algorithms — Perlin's improved noise, and vanilla's octave and
// normal-noise composition on top of it — and checked against cubiomes
// (MIT, Cubitect), the noise reference SPEC §2 names.
//
// WHAT THE VECTORS ESTABLISH: cubiomes is an independent reimplementation of
// Minecraft's noise, so bit-exact agreement with it is strong evidence that
// this code is right. It is not proof that either matches Mojang. That is
// settled by diffing generated terrain against the goldens (M3).
//
// Everything here is bit-exact or it is nothing: the values feed straight
// into terrain shape, so a last-place difference is a different world.

#pragma once

#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace stratum::noise {

/// One octave of improved Perlin noise: a shuffled permutation of 0..255 and
/// an origin offset, sampled over a unit lattice.
class PerlinNoise {
public:
    /// Draws the origin and shuffles the permutation, consuming the
    /// generator exactly as vanilla does: three doubles, then 256 bounded
    /// draws.
    [[nodiscard]] static PerlinNoise fromRandom(rng::Xoroshiro128PlusPlus& random);

    /// The same construction from a Java LCG, which is what a dimension
    /// declaring `legacy_random_source` uses. The draw sequence is identical;
    /// only the generator differs, and the two produce entirely different
    /// permutations from the same seed.
    [[nodiscard]] static PerlinNoise fromRandom(rng::JavaRandom& random);

    PerlinNoise(double originX, double originY, double originZ,
                std::array<std::uint8_t, 256> permutation) noexcept;

    [[nodiscard]] double sample(double x, double y, double z) const noexcept;

    /// The variant that folds the y coordinate back onto a slab, used where
    /// vanilla wants a column-invariant noise.
    [[nodiscard]] double sample(double x, double y, double z, double yScale,
                                double yMax) const noexcept;

    /// 2D simplex noise over the same permutation, which is what the End's
    /// island noise samples.
    [[nodiscard]] double sampleSimplex2D(double x, double y) const noexcept;

    [[nodiscard]] double originX() const noexcept { return originX_; }

    [[nodiscard]] double originY() const noexcept { return originY_; }

    [[nodiscard]] double originZ() const noexcept { return originZ_; }

    [[nodiscard]] const std::array<std::uint8_t, 256>& permutation() const noexcept {
        return permutation_;
    }

private:
    /// Wraps into the 256-entry table. The index is signed because the
    /// lattice coordinates are: a negative one wraps exactly as the
    /// reference's `0xff & i` does, and masking before widening keeps that
    /// explicit rather than routing it through a huge size_t.
    [[nodiscard]] std::uint8_t at(std::int32_t index) const noexcept {
        return permutation_[static_cast<std::size_t>(index & 0xFF)];
    }

    double originX_ = 0.0;
    double originY_ = 0.0;
    double originZ_ = 0.0;
    std::array<std::uint8_t, 256> permutation_{};
};

/// A stack of Perlin octaves with per-octave amplitude and frequency. This is
/// what vanilla calls PerlinNoise; each octave is seeded from the MD5 of
/// "octave_<n>" so that adding or removing octaves does not disturb the rest.
class OctaveNoise {
public:
    /// @p firstOctave is vanilla's `firstOctave`, at most 0. Amplitudes of
    /// zero are skipped, but still advance the frequency and persistence,
    /// which is what makes a sparse amplitude list meaningful.
    [[nodiscard]] static OctaveNoise create(rng::Xoroshiro128PlusPlus& random, int firstOctave,
                                            std::span<const double> amplitudes);

    [[nodiscard]] double sample(double x, double y, double z) const noexcept;

    [[nodiscard]] std::size_t octaveCount() const noexcept { return octaves_.size(); }

private:
    struct Octave {
        PerlinNoise noise;
        double amplitude = 0.0;
        double frequency = 0.0;
    };

    std::vector<Octave> octaves_;
};

/// Vanilla's NormalNoise: two octave stacks, the second sampled at a slightly
/// different frequency, scaled so the result sits in roughly [-1, 1].
class NormalNoise {
public:
    [[nodiscard]] static NormalNoise create(rng::Xoroshiro128PlusPlus& random, int firstOctave,
                                            std::span<const double> amplitudes);

    [[nodiscard]] double sample(double x, double y, double z) const noexcept;

    [[nodiscard]] double valueFactor() const noexcept { return valueFactor_; }

private:
    NormalNoise(OctaveNoise first, OctaveNoise second, double valueFactor)
        : first_(std::move(first)), second_(std::move(second)), valueFactor_(valueFactor) {}

    OctaveNoise first_;
    OctaveNoise second_;
    double valueFactor_ = 1.0;
};

} // namespace stratum::noise
