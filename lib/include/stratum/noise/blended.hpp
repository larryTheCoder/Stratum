// Stratum — the legacy "blended" 3D noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// What `minecraft:old_blended_noise` samples: two sixteen-octave limit
// noises and an eight-octave blend between them, the shape terrain had
// before 1.18 and still the backbone of every dimension's `base_3d_noise`.
//
// Adapted from cubiomes' `sampleSurfaceNoise` and `octaveInit`
// (https://github.com/Cubitect/cubiomes, MIT, noise.c and biomenoise.c),
// which is the reference SPEC §2 names. See NOTICE.
//
// WHAT IS AND IS NOT ESTABLISHED. This is the sharpest example in the
// project so far of an oracle that covers some of a computation and not all
// of it, so it is worth being exact:
//
//   * The octave loop, the per-octave scaling, and the final
//     `clampedLerp(0.5 + 0.05*blend, min/512, max/512)` are checked
//     bit-exactly against cubiomes.
//   * The legacy seeding — sixteen, sixteen and eight Perlin octaves drawn
//     in that order from one Java LCG — is checked against cubiomes too.
//   * `smearScaleMultiplier` is **not**. cubiomes models the pre-1.18 noise,
//     which had no such parameter, so agreement only pins the multiplier-of-
//     one case. Vanilla's own data uses 8.0 and 4.0, and where the number
//     enters the formula is a guess this code makes and does not verify.
//   * The wrap in maintainPrecision is not checked either: cubiomes has it
//     commented out as "useless in practice", so the two agree only while
//     no coordinate reaches the wrap.
//   * How a dimension that does *not* declare `legacy_random_source` seeds
//     these octaves is now *partly* known, and nothing in this file
//     implements it. A search against the deepslate vectors puts the
//     derivation at
//     `XoroshiroPositionalFactory(seed).fromHashOf("minecraft:terrain")`,
//     drawn sequentially in the order min, max, main — each part supported
//     by its own control (SPEC §11). What is still missing is whatever makes
//     the sampling formula agree: that search plateaus at a correlation of
//     0.809, and nothing swept so far moves it.
//
// Which is why `old_blended_noise` remains refused by the interpreter. This
// is the verified half of it, not the whole (SPEC §11).

#pragma once

#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stratum::noise {

/// Vanilla's coordinate wrap, applied before every octave is sampled: it
/// folds a coordinate back into ±2^25 so that the lattice arithmetic keeps
/// its precision far from the origin. Identity for everything nearer than
/// that, which is why cubiomes leaves it out and why the two still agree.
[[nodiscard]] double maintainPrecision(double value) noexcept;

class BlendedNoise {
public:
    /// The five numbers `old_blended_noise` carries. The scales are
    /// multiplied by 684.412 — vanilla's base frequency for this noise —
    /// before use, so a scale of one is not a frequency of one.
    struct Parameters {
        double xzScale = 1.0;
        double yScale = 1.0;
        double xzFactor = 80.0;
        double yFactor = 160.0;
        /// How far the y coordinate is smeared onto a slab per octave.
        /// Vanilla's data uses 8.0 in the overworld and Nether and 4.0 in
        /// the End. See Smear for what it actually does, which is not what
        /// the pre-1.18 function did with the same field.
        double smearScaleMultiplier = 1.0;
    };

    /// Which reading of the function to use. The pre-1.18 one and vanilla
    /// 1.21.11's differ in two places — the normalisation and the smear —
    /// and SPEC §11 records how each was established rather than assumed.
    enum class Reading : std::uint8_t {
        /// The pre-1.18 function, which cubiomes models and the vectors pin
        /// bit-for-bit. Its octave stack is divided by 512, leaving a value
        /// of order a hundred — the density space the old generator worked
        /// in. It had no smear multiplier at all, so what this build does
        /// with one is an extrapolation and is measurably not vanilla's.
        PreModern,
        /// Vanilla 1.21.11's, measured. It divides the octave stack by
        /// 65536 = 2^16 — the sum of the sixteen doubling amplitudes, so the
        /// value lands in the order-one space `sloped_cheese` needs — and it
        /// differs in the cap handed to the Perlin sampler, twice:
        ///
        ///   * The cap grows with y using the slab width taken *before* the
        ///     multiplier is applied. The slab still widens with the
        ///     multiplier; the cap does not. That is what keeps the field
        ///     almost unmoved across a factor of sixteen in the multiplier,
        ///     which vanilla's is and PreModern's is not.
        ///   * Below y = 0 the cap does not bind at all, so the fold runs at
        ///     full effect. Vanilla's spread below zero is flat in y at the
        ///     value its rising curve above zero reaches around y = 240 —
        ///     the signature of saturation, not of a mirrored cap.
        ///
        /// At a multiplier of one, and once the divisors are taken out, the
        /// two are bit-identical for y >= 0 and differ only below it.
        Modern,
    };

    /// The octave counts are fixed by the algorithm, not configurable.
    static constexpr std::size_t kLimitOctaves = 16;
    static constexpr std::size_t kBlendOctaves = 8;

    /// Seeded from a Java LCG, which is what a dimension declaring
    /// `legacy_random_source` uses: the two limit stacks then the blend
    /// stack, drawn in that order from one generator, so that reordering
    /// them changes all three.
    [[nodiscard]] static BlendedNoise legacy(rng::JavaRandom& random, Parameters parameters);

    /// Vanilla 1.21.11's, for a dimension that does not declare
    /// `legacy_random_source` — which the overworld, the Nether and the End
    /// all are.
    ///
    /// The seeding: one generator, taken from the world seed's positional
    /// factory under the name `minecraft:terrain`, and the three stacks drawn
    /// from it in order — sixteen minimum, sixteen maximum, eight blend. Every
    /// part of that is load-bearing and every part was checked against
    /// deepslate's exact values: a different salt, a different order, or the
    /// world seed used directly all miss by order one rather than narrowly
    /// (SPEC §11).
    [[nodiscard]] static BlendedNoise modern(std::int64_t worldSeed, Parameters parameters);

    /// The modern *reading* on the legacy *seeding*. Not something a world
    /// uses: it exists so the tests can hold the seeding constant while
    /// comparing the two readings, which is the only way that comparison
    /// means anything.
    [[nodiscard]] static BlendedNoise withModernReading(rng::JavaRandom& random,
                                                        Parameters parameters);

    [[nodiscard]] Reading reading() const noexcept { return reading_; }

    [[nodiscard]] double sample(double x, double y, double z) const noexcept;

    [[nodiscard]] const Parameters& parameters() const noexcept { return parameters_; }

private:
    BlendedNoise() = default;

    [[nodiscard]] static BlendedNoise build(rng::JavaRandom& random, Parameters parameters,
                                            Reading reading);

    std::vector<PerlinNoise> minimum_;
    std::vector<PerlinNoise> maximum_;
    std::vector<PerlinNoise> blend_;
    Parameters parameters_;
    Reading reading_ = Reading::PreModern;
};

} // namespace stratum::noise
