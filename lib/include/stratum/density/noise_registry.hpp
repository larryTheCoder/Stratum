// Stratum — the noises a density function graph names.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A `noise` field in a density function usually names a `worldgen/noise`
// entry rather than carrying one: an entry has a first octave and a list of
// amplitudes, and this is where the ones a graph names are built. Turning
// them into sampleable NormalNoises needs the world seed, so this is the
// first place in the pipeline where the seed appears.
//
// The other spelling — parameters written inline in the density function —
// is legal and is kept by the graph, but is NOT built here. It has no
// identifier, and an identifier is exactly what the seeding chain below
// consumes; see stratum::density::Interpreter, which refuses such a node by
// name rather than seeding it from a guess (SPEC §11).
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
#include <stratum/density/noise_parameters.hpp>
#include <stratum/noise/perlin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace stratum::density {

/// Which generator seeds a dimension's noises. A noise settings entry
/// chooses one for all of them at once, through `legacy_random_source`, so
/// the same `minecraft:temperature` is a different noise in the overworld
/// than it is in the Nether — and a registry can only hold one of the two.
///
/// Named rather than a bool, and required rather than defaulted, because a
/// default is exactly how this went wrong: the registry used to be built
/// per pack and always with Xoroshiro, which was right for three of
/// vanilla's seven dimensions and silently wrong for the other four.
enum class RandomSource : std::uint8_t {
    /// Xoroshiro128++ through the positional factory, salted with the MD5 of
    /// each noise's identifier. What a dimension declaring
    /// `legacy_random_source: false` uses, which is the overworld and its
    /// two variants.
    Xoroshiro,
    /// The Java LCG. What the Nether, the End, caves and floating islands
    /// use — and what this build cannot yet derive, because nothing
    /// available says how a name becomes an LCG seed here (SPEC §11).
    Legacy,
};

[[nodiscard]] std::string_view randomSourceName(RandomSource source) noexcept;

/// The NormalNoise instances a graph's `noise` fields name, built once for a
/// world seed and immutable afterwards (SPEC §4.1).
class NoiseRegistry {
public:
    /// Builds every noise in @p wanted from @p pack. A name the pack does
    /// not define is an error here rather than on the chunk that first
    /// reached it — which is the whole reason Graph::referencedNoises()
    /// exists.
    ///
    /// @p source has no default on purpose. Throws NoiseError for
    /// RandomSource::Legacy: this build cannot derive it, and it will not
    /// quietly substitute the modern derivation, which would produce four of
    /// vanilla's seven dimensions in a world that generates and is wrong.
    [[nodiscard]] static NoiseRegistry create(const data::Pack& pack,
                                              std::span<const data::ResourceLocation> wanted,
                                              std::int64_t worldSeed, RandomSource source);

    [[nodiscard]] const noise::NormalNoise* find(const data::ResourceLocation& id) const noexcept;

    /// The noise with this identifier. Throws NoiseError naming it if absent.
    [[nodiscard]] const noise::NormalNoise& get(const data::ResourceLocation& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return noises_.size(); }

    [[nodiscard]] std::int64_t worldSeed() const noexcept { return worldSeed_; }

    [[nodiscard]] RandomSource source() const noexcept { return source_; }

private:
    std::map<data::ResourceLocation, noise::NormalNoise> noises_;
    std::int64_t worldSeed_ = 0;
    RandomSource source_ = RandomSource::Xoroshiro;
};

} // namespace stratum::density
