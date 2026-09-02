// Stratum — the noises a density function graph names.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A `noise` field in a density function is an identifier, not a value: it
// names a `worldgen/noise` entry that carries a first octave and a list of
// amplitudes. Turning those into a sampleable NormalNoise needs the world
// seed, so this is the first place in the pipeline where the seed appears.
//
// The seeding chain is the part worth being careful about (CLAUDE.md: "one
// wrong salt/seed derivation shifts everything downstream"):
//
//     world seed -> two draws -> a 128-bit base
//     base XOR md5("minecraft:continentalness") -> that noise's generator
//
// so every noise is a pure function of the world seed and its own name, and
// naming one differently changes only that one. Checked against cubiomes'
// setBiomeSeed — see tools/vectors/climate_vectors.c.

#pragma once

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/noise/perlin.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::density {

/// Raised when a `worldgen/noise` entry is missing or malformed. Never
/// silently substituted: a noise that quietly defaulted would produce a
/// world that generates but is not the one asked for (SPEC §8).
class NoiseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A `worldgen/noise` entry, as declared.
struct NoiseParameters {
    /// Vanilla's `firstOctave`, the exponent of the lowest frequency.
    int firstOctave = 0;
    /// One amplitude per octave. Zeroes are meaningful: they position the
    /// octaves that follow without contributing themselves.
    std::vector<double> amplitudes;

    /// Parses one entry. @p id names the entry in any error raised.
    [[nodiscard]] static NoiseParameters fromJson(const nlohmann::json& json,
                                                  const data::ResourceLocation& id);
};

/// The NormalNoise instances a graph's `noise` fields name, built once for a
/// world seed and immutable afterwards (SPEC §4.1).
class NoiseRegistry {
public:
    /// Builds every noise in @p wanted from @p pack. A name the pack does
    /// not define is an error here rather than on the chunk that first
    /// reached it — which is the whole reason Graph::referencedNoises()
    /// exists.
    [[nodiscard]] static NoiseRegistry create(const data::Pack& pack,
                                              std::span<const data::ResourceLocation> wanted,
                                              std::int64_t worldSeed);

    [[nodiscard]] const noise::NormalNoise* find(const data::ResourceLocation& id) const noexcept;

    /// The noise with this identifier. Throws NoiseError naming it if absent.
    [[nodiscard]] const noise::NormalNoise& get(const data::ResourceLocation& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return noises_.size(); }

    [[nodiscard]] std::int64_t worldSeed() const noexcept { return worldSeed_; }

private:
    std::map<data::ResourceLocation, noise::NormalNoise> noises_;
    std::int64_t worldSeed_ = 0;
};

} // namespace stratum::density
