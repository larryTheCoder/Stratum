// Stratum — the noises a density function graph names.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ADAPTED (CLAUDE.md, NOTICE): two seedings here are adapted from cubiomes
// (https://github.com/Cubitect/cubiomes, MIT License, Copyright (c) 2020
// Cubitect), biomenoise.c, read at e61f90580cbdd883214a8054670dacae655e59c0.
//
//   * The legacy climate — the seeds worldSeed and worldSeed + 1 and the
//     fixed two-octave stacks from octave -7 that `legacyClimateNoise`
//     builds — is `setNetherSeed`. `minecraft:offset` as zero is this
//     project's reading of `getNetherBiome` sampling the climate unshifted,
//     not code taken from it.
//   * The modern path at the end of `NoiseRegistry::create` — one
//     XoroshiroPositionalFactory built from the world seed, then each noise's
//     generator salted with the MD5 of its identifier — is `setBiomeSeed`
//     with `init_climate_seed` (rng/xoroshiro128.hpp says what of it is
//     whose). The parameters each noise is built from are the pack's, where
//     cubiomes hardcodes its climate noises'.
//
// Recorded in NOTICE.

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stratum::density {

std::string_view randomSourceName(RandomSource source) noexcept {
    switch (source) {
        case RandomSource::Xoroshiro:
            return "xoroshiro";
        case RandomSource::Legacy:
            return "legacy";
    }
    return "unknown";
}

namespace {

/// Whether @p id is one of the router's three legacy climate keys (below).
[[nodiscard]] bool isLegacyClimateNoise(const data::ResourceLocation& id);

} // namespace

NoiseRegistry NoiseRegistry::create(const data::Pack& pack,
                                    std::span<const data::ResourceLocation> wanted,
                                    std::int64_t worldSeed, RandomSource source) {
    std::map<data::ResourceLocation, NoiseParameters> parameters;
    for (const data::ResourceLocation& id : wanted) {
        if (parameters.contains(id)) {
            continue;
        }
        const data::PackEntry* entry = pack.find(data::Registry::Noise, id);
        if (entry == nullptr) {
            // Under Legacy the router's three climate keys ignore the pack
            // (the overload below), so a pack may leave them out; only a
            // named-path read of one needs them, and getNamed() says so.
            if (source == RandomSource::Legacy && isLegacyClimateNoise(id)) {
                continue;
            }
            throw NoiseError("the pack defines no noise '" + id.toString() +
                             "', which a density function references");
        }
        parameters.emplace(id, NoiseParameters::fromJson(entry->json, id));
    }
    return create(parameters, wanted, worldSeed, source);
}

namespace {

// THE LEGACY CLIMATE, read from cubiomes and measured against the goldens
// (SPEC §11, "The legacy Nether's climate, read from cubiomes"). Adapted from
// cubiomes (https://github.com/Cubitect/cubiomes, MIT, Cubitect),
// biomenoise.c `setNetherSeed`, at e61f905.
//
// Under the Java LCG, the three climate noises a legacy router names are NOT
// built from the pack's parameters, and no name is hashed:
//
//   minecraft:temperature  java.util.Random(worldSeed)
//   minecraft:vegetation   java.util.Random(worldSeed + 1)
//   both                   two octaves at a fixed first octave of -7
//   minecraft:offset       zero everywhere: the climate takes no shift
//
// Measured, through this registry and the shipped Interpreter, against all
// eight golden Nether regions: 32765 of 32768 cells pick vanilla's biome
// (131061 of 131072 at every chunk), and every cell that does not is an exact
// fitness tie lost to ParameterList::find's tie-break proxy, not a climate
// value that differs (vanilla_legacy_nether_climate_gap_test.cpp).
//
// TWO READINGS ARE CARRIED, NOT MEASURED, and SPEC §11 states both:
//   * the rule is keyed on the noise's NAME under a legacy source, which is
//     all a registry can see. The Nether cannot tell that apart from keying
//     on the dimension, and caves and floating_islands — which name the same
//     three — take it with no oracle of their own;
//   * the pack's parameters for these three are ignored, as vanilla's own
//     shipped ones are. Whether a datapack that REDEFINES them would move a
//     legacy dimension's climate is unmeasured.
constexpr int kLegacyClimateFirstOctave = -7;
constexpr int kLegacyClimateOctaves = 2;

[[nodiscard]] const data::ResourceLocation& legacyTemperature() {
    static const data::ResourceLocation kId =
        data::ResourceLocation::parse("minecraft:temperature");
    return kId;
}

[[nodiscard]] const data::ResourceLocation& legacyVegetation() {
    static const data::ResourceLocation kId = data::ResourceLocation::parse("minecraft:vegetation");
    return kId;
}

[[nodiscard]] const data::ResourceLocation& legacyOffset() {
    static const data::ResourceLocation kId = data::ResourceLocation::parse("minecraft:offset");
    return kId;
}

[[nodiscard]] bool isLegacyClimateNoise(const data::ResourceLocation& id) {
    return id == legacyTemperature() || id == legacyVegetation() || id == legacyOffset();
}

[[nodiscard]] noise::NormalNoise legacyClimateNoise(const data::ResourceLocation& id,
                                                    std::int64_t worldSeed) {
    if (id == legacyOffset()) {
        return noise::NormalNoise::zero();
    }
    // `worldSeed + 1` in two's complement, as Java's long addition is:
    // Long.MAX_VALUE + 1 is Long.MIN_VALUE, not undefined behaviour.
    rng::JavaRandom random{
        id == legacyTemperature() ? worldSeed : javamath::wrappingAdd(worldSeed, std::int64_t{1})};
    return noise::NormalNoise::createLegacy(random, kLegacyClimateFirstOctave,
                                            kLegacyClimateOctaves);
}

} // namespace

NoiseRegistry
NoiseRegistry::create(const std::map<data::ResourceLocation, NoiseParameters>& parameters,
                      std::span<const data::ResourceLocation> wanted, std::int64_t worldSeed,
                      RandomSource source) {
    NoiseRegistry registry;
    registry.worldSeed_ = worldSeed;
    registry.source_ = source;

    if (source == RandomSource::Legacy) {
        // Every other name: spec/legacy-named-noise-spec.md. One factory seed
        // F(W), the first nextLong of java.util.Random(worldSeed), fixed for
        // the dimension (Q1.2); each noise salted by its own full identifier
        // and built from the pack's parameters (Q1.3, Q2.1), independently of
        // which others are built or in what order (Q1.4). This replaced a
        // refusal by name, which stood while the derivation was unknown: a
        // 729-million-candidate black-box search had found no survivor
        // (SPEC §11), because the octaves are salted one by one rather than
        // drawn from one stream. The three climate keys keep their built-in
        // construction above for the router, with the named construction
        // held beside it for surface rules (Q5.2).
        rng::JavaRandom world{worldSeed};
        const std::int64_t factorySeed = world.nextLong();
        for (const data::ResourceLocation& id : wanted) {
            if (registry.noises_.contains(id)) {
                continue;
            }
            if (isLegacyClimateNoise(id)) {
                registry.noises_.emplace(id, legacyClimateNoise(id, worldSeed));
                // Q5.2: the named path's noise under the same key, held apart.
                const auto named = parameters.find(id);
                if (named != parameters.end()) {
                    registry.namedClimate_.emplace(
                        id, noise::NormalNoise::createNamedLegacy(factorySeed, id.toString(),
                                                                  named->second.firstOctave,
                                                                  named->second.amplitudes));
                }
                continue;
            }
            const auto found = parameters.find(id);
            if (found == parameters.end()) {
                throw NoiseError("no parameters for noise '" + id.toString() +
                                 "', which a density function or surface rule references");
            }
            registry.noises_.emplace(id, noise::NormalNoise::createNamedLegacy(
                                             factorySeed, id.toString(), found->second.firstOctave,
                                             found->second.amplitudes));
        }
        return registry;
    }

    // Forked once, then salted per name: cubiomes' setBiomeSeed and
    // init_climate_seed (adapted; see the header). Building it outside the
    // loop is not an optimisation: the base is defined by two draws from the
    // world seed and nothing else, so drawing again per noise would be a
    // different derivation entirely.
    const rng::XoroshiroPositionalFactory factory{worldSeed};

    for (const data::ResourceLocation& id : wanted) {
        if (registry.noises_.contains(id)) {
            continue;
        }

        const auto found = parameters.find(id);
        if (found == parameters.end()) {
            throw NoiseError("no parameters for noise '" + id.toString() +
                             "', which a density function references");
        }
        rng::Xoroshiro128PlusPlus random = factory.fromHashOf(id.toString());
        registry.noises_.emplace(id, noise::NormalNoise::create(random, found->second.firstOctave,
                                                                found->second.amplitudes));
    }
    return registry;
}

bool NoiseRegistry::isLegacyRouterClimateKey(const data::ResourceLocation& id) {
    return isLegacyClimateNoise(id);
}

void NoiseRegistry::requireLegacyRouterSettled(
    const std::span<const data::ResourceLocation> routerNoises, const RandomSource source) {
    if (source != RandomSource::Legacy) {
        return;
    }
    std::vector<std::string> unsettled;
    for (const data::ResourceLocation& id : routerNoises) {
        const std::string name = id.toString();
        if (!isLegacyClimateNoise(id) &&
            std::find(unsettled.begin(), unsettled.end(), name) == unsettled.end()) {
            unsettled.push_back(name);
        }
    }
    if (unsettled.empty()) {
        return;
    }
    std::string names;
    for (const std::string& name : unsettled) {
        names += (names.empty() ? "" : ", ") + name;
    }
    throw NoiseError("this dimension declares legacy_random_source and its noise router names " +
                     std::to_string(unsettled.size()) + " noise(s) — " + names +
                     " — beyond minecraft:temperature, minecraft:vegetation and "
                     "minecraft:offset. How the router builds those is read in "
                     "spec/legacy-named-noise-spec.md Q5.1c but not confirmed against the "
                     "server, so this build refuses them rather than generate on the spec's "
                     "word (SPEC §8)");
}

const noise::NormalNoise* NoiseRegistry::find(const data::ResourceLocation& id) const noexcept {
    const auto found = noises_.find(id);
    return found == noises_.end() ? nullptr : &found->second;
}

const noise::NormalNoise*
NoiseRegistry::findNamed(const data::ResourceLocation& id) const noexcept {
    if (source_ == RandomSource::Legacy && isLegacyClimateNoise(id)) {
        const auto found = namedClimate_.find(id);
        return found == namedClimate_.end() ? nullptr : &found->second;
    }
    return find(id);
}

const noise::NormalNoise& NoiseRegistry::getNamed(const data::ResourceLocation& id) const {
    const noise::NormalNoise* noise = findNamed(id);
    if (noise == nullptr) {
        throw NoiseError("no noise '" + id.toString() +
                         "' was built for the named path (a surface rule's noise) for this world "
                         "seed; under legacy_random_source it needs the pack's own parameters "
                         "for that key (spec/legacy-named-noise-spec.md Q5.2)");
    }
    return *noise;
}

const noise::NormalNoise& NoiseRegistry::get(const data::ResourceLocation& id) const {
    const noise::NormalNoise* noise = find(id);
    if (noise == nullptr) {
        throw NoiseError("no noise '" + id.toString() + "' was built for this world seed");
    }
    return *noise;
}

} // namespace stratum::density
