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
// of it, so it is worth being exact. This section is HISTORICAL — it
// describes the state before the Modern reading and the modern seeding
// (below) landed, kept because the gaps it names turned out to matter:
//
//   * The octave loop, the per-octave scaling, and the final
//     `clampedLerp(0.5 + 0.05*blend, min/512, max/512)` are checked
//     bit-exactly against cubiomes.
//   * The legacy seeding — sixteen, sixteen and eight Perlin octaves drawn
//     in that order from one Java LCG — is checked against cubiomes too.
//   * `smearScaleMultiplier` is **not**. cubiomes models the pre-1.18 noise,
//     which had no such parameter, so agreement only pins the multiplier-of-
//     one case. Vanilla's own data uses 8.0 and 4.0, and where the number
//     enters the formula was a GUESS — flagged here as unverified before
//     anyone had a way to check it end-to-end. It is now measurably WRONG:
//     a wrapped-`range_choice` probe reading vanilla's own `base_3d_noise`
//     directly (SPEC §11, the M3 residual entry) finds this build's Modern
//     reading disagreeing with the server at several corners, spread across
//     unrelated columns and elevations — not a location-specific glitch.
//     The fold/cap mechanism below (`smearCap`, and the corresponding fold
//     in `PerlinNoise::sample`) is the named suspect; the exact mistake is
//     still open.
//   * The wrap in maintainPrecision is not checked either: cubiomes has it
//     commented out as "useless in practice", so the two agree only while
//     no coordinate reaches the wrap.
//
// The modern seeding this section called "partly known" and stuck at a
// correlation of 0.809 is SETTLED below (`BlendedNoise::modern`, `Reading`):
// `XoroshiroPositionalFactory(seed).fromHashOf("minecraft:terrain")`, every
// part checked against deepslate's exact values (SPEC §11). `old_blended_noise`
// is no longer refused by the interpreter — it is evaluated via the Modern
// reading — but per the smearScaleMultiplier finding above, "evaluated" is
// not yet "verified everywhere."

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

    /// The legacy *seeding*: the two limit stacks then the blend stack,
    /// drawn in that order from one Java LCG, so that reordering them
    /// changes all three.
    ///
    /// Note which reading this pairs it with. It uses PreModern — the
    /// pre-1.18 function, which cubiomes models and the vectors pin
    /// bit-for-bit — and that is **not** what a 1.21.11 dimension declaring
    /// `legacy_random_source` produces. That is this same seeding under the
    /// *modern* reading; see `legacyFromWorldSeed` below. The two differ by
    /// exactly 128x at y = 0, so the choice between them is not a rounding
    /// question. Kept as it is because the vectors are what pin the octave
    /// loop itself.
    [[nodiscard]] static BlendedNoise legacy(rng::JavaRandom& random, Parameters parameters);

    /// Vanilla 1.21.11's, for a dimension that does not declare
    /// `legacy_random_source` — which is the overworld and its two variants,
    /// `amplified` and `large_biomes`, and those only. `nether`, `end`,
    /// `caves` and `floating_islands` all declare it true (read out of the
    /// 1.21.11 fixtures, not assumed; an earlier version of this comment
    /// claimed the opposite for the Nether and the End and was wrong).
    ///
    /// The seeding: one generator, taken from the world seed's positional
    /// factory under the name `minecraft:terrain`, and the three stacks drawn
    /// from it in order — sixteen minimum, sixteen maximum, eight blend. Every
    /// part of that is load-bearing and every part was checked against
    /// deepslate's exact values: a different salt, a different order, or the
    /// world seed used directly all miss by order one rather than narrowly
    /// (SPEC §11).
    [[nodiscard]] static BlendedNoise modern(std::int64_t worldSeed, Parameters parameters);

    /// The modern *reading* on the legacy *seeding*, taking the generator
    /// rather than a world seed. It was introduced as a test-only lever for
    /// holding the seeding constant while comparing the two readings; that
    /// combination then turned out to be vanilla's own, so the rule a world
    /// actually uses has its own name below and this stays the lever.
    [[nodiscard]] static BlendedNoise withModernReading(rng::JavaRandom& random,
                                                        Parameters parameters);

    /// What a dimension declaring `legacy_random_source` uses for its
    /// `old_blended_noise`: the world seed handed straight to a Java LCG —
    /// no positional fork, no name salt, nothing derived — with the three
    /// stacks drawn in `legacy`'s order and the value read the **modern**
    /// way.
    ///
    /// MEASURED, NOT DERIVED, off the vanilla server through
    /// `tools/analysis/legacy-blended-probe.sh`; scored by
    /// `tools/analysis/legacy-blended-analyze.cpp` and pinned by
    /// `tests/conformance/vanilla_legacy_blended_test.cpp`. The counts and
    /// the rivals it beat are in SPEC §11 rather than here, so that one copy
    /// of them can be kept current.
    ///
    /// Nothing reaches this for a real world yet. `NoiseRegistry::create`
    /// still refuses `RandomSource::Legacy` outright, because a legacy
    /// dimension's *named* noises remain underived (see its comment, and
    /// SPEC §11) — and that refusal comes first, before any density function
    /// is built. So `Interpreter` selecting this on a Legacy registry is
    /// correct-and-unreachable: it is the half of the answer that is
    /// settled, written where it belongs, so that lifting the refusal does
    /// not also have to rediscover this.
    [[nodiscard]] static BlendedNoise legacyFromWorldSeed(std::int64_t worldSeed,
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
