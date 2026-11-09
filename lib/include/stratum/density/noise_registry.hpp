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
// naming one differently changes only that one. Adapted from cubiomes'
// (MIT) setBiomeSeed and init_climate_seed, as is the legacy climate from its
// setNetherSeed — noise_registry.cpp's header and NOTICE record both — and
// checked against them: see tools/vectors/climate_vectors.c.

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
    /// the golden Nether). Every OTHER name under Legacy is built by
    /// spec/legacy-named-noise-spec.md from the pack's own parameters:
    /// `java.util.Random` salted with the identifier's sign-extended
    /// String.hashCode XOR the dimension's factory seed (the first nextLong of
    /// `java.util.Random(worldSeed)`), then one generator per octave salted
    /// with `octave_<n>` (noise::NormalNoise::createNamedLegacy). Under either
    /// source, a name the parameters do not define throws NoiseError naming
    /// it.
    ///
    /// The three climate keys are special only as the ROUTER reads them
    /// (Q5.1c). A surface rule or the surface system asking for one of them
    /// under Legacy gets the named construction instead (Q5.2), a different
    /// noise under the same key, so the two are held apart and never shared
    /// (IMPLEMENTER-RULING-01): get() and find() answer for the router, and
    /// getNamed() and findNamed() for the named path. Where the pack
    /// defines parameters for one of the three, its named construction is
    /// built beside the router's; findNamed() returns null, and getNamed()
    /// throws naming the key, where it does not. Under Xoroshiro, and for
    /// every other key, the two paths are the same noise.
    ///
    /// A NAMED NOISE IS NOT THE ONLY THING A LEGACY SOURCE SEEDS. A
    /// dimension also draws on its declared random source for
    /// `minecraft:vertical_gradient`'s random_name, the aquifer lattice and
    /// the ore-vein source, none of which appears in @p wanted. All three
    /// are measured under the legacy source and derived from the declared
    /// source where they are built: surface::Executor::compile reads it from
    /// this registry, and the aquifer's CentreSource and the ore::VeinSource
    /// take it from terrain::ChunkFiller::compile (SPEC §11). Each was
    /// refused by name there until its measurement landed.
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

    /// The noise a NAMED-path consumer — a surface rule's noise_threshold,
    /// or the surface system's own keys — gets under this identifier. The
    /// same as find()/get() except for the three router climate keys under
    /// Legacy (see create()).
    [[nodiscard]] const noise::NormalNoise*
    findNamed(const data::ResourceLocation& id) const noexcept;
    [[nodiscard]] const noise::NormalNoise& getNamed(const data::ResourceLocation& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return noises_.size(); }

    [[nodiscard]] std::int64_t worldSeed() const noexcept { return worldSeed_; }

    [[nodiscard]] RandomSource source() const noexcept { return source_; }

    /// `minecraft:temperature`, `minecraft:vegetation` and `minecraft:offset`:
    /// under Legacy, the keys whose noise is the router's built-in climate
    /// rather than the named construction (spec/legacy-named-noise-spec.md
    /// Q5.1c).
    [[nodiscard]] static bool isLegacyRouterClimateKey(const data::ResourceLocation& id);

    /// Throws NoiseError, naming them, if @p routerNoises — the noises a
    /// dimension's noise ROUTER reaches — holds any key other than the three
    /// climate keys under Legacy. spec/legacy-named-noise-spec.md says the
    /// router builds those by the named construction too (Q5.1c), but only
    /// as read, and no golden confirms it: vanilla's legacy routers name the
    /// climate keys alone. Until one does, that path stays refused
    /// (IMPLEMENTER-RULING-01 §2). Nothing under Xoroshiro.
    static void requireLegacyRouterSettled(std::span<const data::ResourceLocation> routerNoises,
                                           RandomSource source);

private:
    std::map<data::ResourceLocation, noise::NormalNoise> noises_;
    /// Under Legacy only: the named-path construction of the three router
    /// climate keys, where the pack defines their parameters.
    std::map<data::ResourceLocation, noise::NormalNoise> namedClimate_;
    std::int64_t worldSeed_ = 0;
    RandomSource source_ = RandomSource::Xoroshiro;
};

} // namespace stratum::density
