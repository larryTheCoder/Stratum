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
#include <stratum/density/random_source.hpp>
#include <stratum/noise/perlin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace stratum::density {

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
    /// @p source has no default on purpose. Under RandomSource::Legacy,
    /// `minecraft:temperature` and `minecraft:vegetation` are built from
    /// `java.util.Random(worldSeed)` and `(worldSeed + 1)` as two-octave
    /// noises at first octave -7, and `minecraft:offset` is zero — the pack's
    /// parameters for those three are not read (SPEC §11, measured against
    /// the golden Nether). Throws NoiseError if @p wanted names ANY OTHER
    /// noise under Legacy: this build cannot derive how a NAME becomes an
    /// LCG seed, and it will not quietly substitute the modern derivation,
    /// which would produce a world that generates and is wrong. Those names,
    /// and only those, go in the message, because which noises a dimension
    /// still cannot have is the actionable part of the refusal (SPEC §8).
    ///
    /// An EMPTY @p wanted under Legacy is not refused, and that is not a
    /// loosening of the rule but the rule stated exactly: with no identifier
    /// to hash there is nothing to derive and nothing to approximate. It is
    /// the case that matters, because thirteen of a legacy dimension's
    /// fifteen router entries — `final_density` among them — name no noise
    /// at all, so a legacy dimension's TERRAIN builds from an empty registry
    /// — and, by the climate rule above, its climate builds too — while its
    /// surface rules stay refused. The refusal used to
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
    /// the ore-vein source, none of which appears in @p wanted. The first two
    /// are measured and derived from the declared source where they are
    /// built (surface::Executor::compile reads it from this registry; the
    /// aquifer's CentreSource takes it). The vein source is refused BY NAME
    /// OF THE CONSTRUCT in terrain::ChunkFiller::compile, where it is
    /// constructed. See legacyConstructRefusal() below, and SPEC §11.
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
/// through `wanted`. The aquifer's centres and the gradient's coin are
/// derived from the declared source, both measured (SPEC §11): the same
/// legacy primitive, found on a constructed aquifer probe and exact out of
/// sample on the golden Nether's bedrock. The vein source is still derived
/// with Xoroshiro128++ only, and is the one construct this message is for.
///
/// Substituting the modern derivation is MEASURED WRONG, not merely
/// unverified, on the one construct where it was measured: against the
/// golden NETHER's bedrock floor and roof the Xoroshiro gradient agrees at
/// chance, while the identical code on the modern overworld is exact — see
/// tests/conformance/vanilla_legacy_gradient_gap_test.cpp, which carries the
/// numbers and their denominators. The ore source is UNTESTED: every vanilla
/// legacy dimension has the flag off, and no probe has asked for one.
///
/// So it is refused BY NAME, wherever it is built, and the refusal says
/// which construct and why. A legacy dimension that names no noise but
/// placed vanilla's bedrock gradient once compiled and filled a chunk with
/// bedrock from the Xoroshiro coin, with no error and no warning — the
/// plausible-but-wrong world SPEC §8 forbids, closed first by refusing the
/// gradient and since by running the measured legacy coin.
[[nodiscard]] std::string legacyConstructRefusal(std::string_view construct,
                                                 std::string_view detail);

} // namespace stratum::density
