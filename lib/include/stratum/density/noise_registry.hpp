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
// is legal and is kept by the graph, but is NOT built here, and nothing is
// missing. It has no identifier, and an identifier is exactly what the
// seeding chain below consumes; the vanilla server has the same problem and
// resolves it by refusing to build the world at all, which
// tools/analysis/inline-noise-probe.sh measured. See
// stratum::density::Interpreter, which refuses such a node by name (SPEC §11).
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
#include <string>
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
    /// use. What this build cannot derive is the seeding of a NAMED noise,
    /// because nothing available says how a name becomes an LCG seed here.
    /// The nameless one, `old_blended_noise`, IS derived — the world seed
    /// handed straight to the LCG (SPEC §11) — which is why these four
    /// dimensions' terrain is not blocked by this and their climate is.
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
    /// RandomSource::Legacy WITH A NON-EMPTY @p wanted: this build cannot
    /// derive how a NAME becomes an LCG seed, and it will not quietly
    /// substitute the modern derivation, which would produce a world that
    /// generates and is wrong. The names go in the message, because which
    /// noises a dimension still cannot have is the actionable part of the
    /// refusal now that it is no longer all-or-nothing (SPEC §8).
    ///
    /// An EMPTY @p wanted under Legacy is not refused, and that is not a
    /// loosening of the rule but the rule stated exactly: with no identifier
    /// to hash there is nothing to derive and nothing to approximate. It is
    /// the case that matters, because thirteen of a legacy dimension's
    /// fifteen router entries — `final_density` among them — name no noise
    /// at all, so a legacy dimension's TERRAIN builds from an empty registry
    /// while its climate and surface rules stay refused. The refusal used to
    /// fire before @p wanted was looked at, which made the gap look four
    /// dimensions wide when it is two climate router entries and one surface
    /// rule wide in three of them, and zero wide in the fourth — the End,
    /// which names none in its router or in its surface rule and so never
    /// needed the answer. See
    /// tests/conformance/vanilla_legacy_named_noises_test.cpp, which pins
    /// exactly which legacy dimension names what, per entry, and SPEC §11.
    ///
    /// A NAMED NOISE IS NOT THE ONLY THING A LEGACY SOURCE SEEDS, and this
    /// function no longer carries the whole refusal on its own. A dimension
    /// also draws on its declared random source for
    /// `minecraft:vertical_gradient`'s random_name, the aquifer lattice and
    /// the ore-vein source, none of which appears in @p wanted. Those are
    /// refused BY NAME OF THE CONSTRUCT where they are built:
    /// surface::Executor::compile for the gradient, and
    /// terrain::ChunkFiller::compile for both flags, which is where the
    /// aquifer lattice and the vein source are actually constructed. See
    /// legacyConstructRefusal() below, and SPEC §11.
    [[nodiscard]] static NoiseRegistry create(const data::Pack& pack,
                                              std::span<const data::ResourceLocation> wanted,
                                              std::int64_t worldSeed, RandomSource source);

    /// The same, from parameters already read out of a pack — a frozen
    /// pipeline's (SPEC §6), which must generate without the pack it came
    /// from. The pack overload resolves its parameters and then calls this,
    /// so the two cannot derive a noise differently.
    [[nodiscard]] static NoiseRegistry
    create(const std::map<data::ResourceLocation, NoiseParameters>& parameters,
           std::span<const data::ResourceLocation> wanted, std::int64_t worldSeed,
           RandomSource source);

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

/// The refusal message for a CONSTRUCT — not a noise — that a
/// `legacy_random_source` dimension needs a random for, and that this build
/// can only derive the modern way.
///
/// WHY THIS EXISTS SEPARATELY FROM NoiseRegistry::create. Narrowing that
/// refusal to "the dimension names a worldgen/noise" left a hole, and the
/// hole is measured rather than theorised. A legacy dimension also draws on
/// its declared random source for `minecraft:vertical_gradient`'s
/// `random_name`, for the aquifer lattice's centre jitter, and for the
/// ore-vein source — none of which is a named noise and none of which passes
/// through `wanted`. This build derives all three with Xoroshiro128++
/// unconditionally.
///
/// For `vertical_gradient` that is MEASURED WRONG, not merely unverified:
/// against the golden NETHER's bedrock floor and roof the Xoroshiro
/// derivation agrees at chance, while the identical code on the modern
/// overworld is exact — see
/// tests/conformance/vanilla_legacy_gradient_gap_test.cpp, which carries the
/// numbers and their denominators. The aquifer and ore sources are
/// structurally identical draws from the same primitive and are UNTESTED:
/// every vanilla legacy dimension has both flags off, so there is no oracle
/// on disk for them.
///
/// So the constructs are refused BY NAME, wherever they are built, and the
/// refusal says which construct and why. A legacy dimension that names no
/// noise but places vanilla's bedrock gradient used to compile and fill a
/// chunk with bedrock, with no error and no warning — the
/// plausible-but-wrong world SPEC §8 forbids.
[[nodiscard]] std::string legacyConstructRefusal(std::string_view construct,
                                                 std::string_view detail);

} // namespace stratum::density
