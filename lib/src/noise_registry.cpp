// Stratum — the noises a density function graph names.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

NoiseRegistry NoiseRegistry::create(const data::Pack& pack,
                                    std::span<const data::ResourceLocation> wanted,
                                    std::int64_t worldSeed, RandomSource source) {
    if (source == RandomSource::Legacy) {
        // Refused rather than approximated. The modern derivation is not a
        // near-enough stand-in: it would seed every noise differently and
        // produce a Nether that generates and is not vanilla's, with nothing
        // to indicate it (SPEC §8, §11).
        throw NoiseError(
            "this dimension declares legacy_random_source, and how a noise's name becomes a "
            "seed under the Java LCG is not settled here — so its noises cannot be built. This "
            "build will not substitute the modern derivation (SPEC §11)");
    }

    NoiseRegistry registry;
    registry.worldSeed_ = worldSeed;
    registry.source_ = source;

    // Forked once, then salted per name. Building it outside the loop is not
    // an optimisation: the base is defined by two draws from the world seed
    // and nothing else, so drawing again per noise would be a different
    // derivation entirely.
    const rng::XoroshiroPositionalFactory factory{worldSeed};

    for (const data::ResourceLocation& id : wanted) {
        if (registry.noises_.contains(id)) {
            continue;
        }

        const data::PackEntry* entry = pack.find(data::Registry::Noise, id);
        if (entry == nullptr) {
            throw NoiseError("the pack defines no noise '" + id.toString() +
                             "', which a density function references");
        }

        const NoiseParameters parameters = NoiseParameters::fromJson(entry->json, id);
        rng::Xoroshiro128PlusPlus random = factory.fromHashOf(id.toString());
        registry.noises_.emplace(
            id, noise::NormalNoise::create(random, parameters.firstOctave, parameters.amplitudes));
    }

    return registry;
}

const noise::NormalNoise* NoiseRegistry::find(const data::ResourceLocation& id) const noexcept {
    const auto found = noises_.find(id);
    return found == noises_.end() ? nullptr : &found->second;
}

const noise::NormalNoise& NoiseRegistry::get(const data::ResourceLocation& id) const {
    const noise::NormalNoise* noise = find(id);
    if (noise == nullptr) {
        throw NoiseError("no noise '" + id.toString() + "' was built for this world seed");
    }
    return *noise;
}

} // namespace stratum::density
