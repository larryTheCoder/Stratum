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
//     these octaves is unknown here, and nothing in this file answers it.
//
// Which is why `old_blended_noise` remains refused by the interpreter. This
// is the verified half of it, not the whole (SPEC §11).

#pragma once

#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>

#include <cstddef>
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
        /// the End; a value of one is the pre-1.18 behaviour cubiomes
        /// models, and the only one checked here.
        double smearScaleMultiplier = 1.0;
    };

    /// The octave counts are fixed by the algorithm, not configurable.
    static constexpr std::size_t kLimitOctaves = 16;
    static constexpr std::size_t kBlendOctaves = 8;

    /// Seeded from a Java LCG, which is what a dimension declaring
    /// `legacy_random_source` uses: the two limit stacks then the blend
    /// stack, drawn in that order from one generator, so that reordering
    /// them changes all three.
    [[nodiscard]] static BlendedNoise legacy(rng::JavaRandom& random, Parameters parameters);

    [[nodiscard]] double sample(double x, double y, double z) const noexcept;

    [[nodiscard]] const Parameters& parameters() const noexcept { return parameters_; }

private:
    BlendedNoise() = default;

    std::vector<PerlinNoise> minimum_;
    std::vector<PerlinNoise> maximum_;
    std::vector<PerlinNoise> blend_;
    Parameters parameters_;
};

} // namespace stratum::noise
