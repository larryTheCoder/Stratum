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
#include <map>
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
    std::map<data::ResourceLocation, NoiseParameters> parameters;
    if (source == RandomSource::Legacy) {
        // The refusal comes before any lookup, as it always has.
        return create(parameters, wanted, worldSeed, source);
    }
    for (const data::ResourceLocation& id : wanted) {
        if (parameters.contains(id)) {
            continue;
        }
        const data::PackEntry* entry = pack.find(data::Registry::Noise, id);
        if (entry == nullptr) {
            throw NoiseError("the pack defines no noise '" + id.toString() +
                             "', which a density function references");
        }
        parameters.emplace(id, NoiseParameters::fromJson(entry->json, id));
    }
    return create(parameters, wanted, worldSeed, source);
}

NoiseRegistry
NoiseRegistry::create(const std::map<data::ResourceLocation, NoiseParameters>& parameters,
                      std::span<const data::ResourceLocation> wanted, std::int64_t worldSeed,
                      RandomSource source) {
    if (source == RandomSource::Legacy) {
        // Refused rather than approximated. The modern derivation is not a
        // near-enough stand-in: it would seed every noise differently and
        // produce a Nether that generates and is not vanilla's, with nothing
        // to indicate it (SPEC §8, §11).
        //
        // SEARCHED, not merely unattempted — and the searching is in the
        // repository rather than in a memory of it:
        // tools/analysis/legacy-seed-probe.sh generates the worlds and
        // tools/analysis/legacy-seed-analyze.cpp scores candidates against
        // them. What a later attempt should not repeat:
        //
        //   * 270,000 candidates per dimension — 900 seed rules (5 bases x 10
        //     salt spellings x 3 combining operators x 0-2 extra LCG forks x
        //     2 generators) x 300 block offsets — over 9 probe dimensions and
        //     2 world seeds. Not one reached half agreement on the probe
        //     subset. The scan's own header lists the space exactly; the
        //     tool prints the count on every run.
        //   * Among them, at rule 182 block 0, the derivation deepslate uses:
        //     base = JavaRandom(worldSeed).nextLong(), XOR the first eight
        //     bytes of MD5("ns:path"), one further LCG fork. It scores at the
        //     dimension's null.
        //
        // The search is calibrated rather than merely large, in both of the
        // ways it has to be:
        //
        //   * its STATISTIC is sensitive — a candidate planted inside the
        //     space and put through the same quantisation the server's
        //     terrain imposes is returned at rank 1, as the sole survivor, at
        //     2304/2304 columns, in every legacy configuration;
        //   * and its FORWARD MODEL is right — which the plant cannot show,
        //     because a plant synthesises its readings through that same
        //     model and would come back at rank 1 even if the model were
        //     wrong. `--control` scores the modern derivation implemented
        //     below against the probe's flag-off mirror dimensions through
        //     the identical readback: 6912/6912 columns per seed, exactly
        //     through the readback's own inversion, while the same rule sits
        //     at the null (378-379/13824) on the legacy dimensions of the
        //     same worlds, and at worldSeed + 1 falls to the null on the
        //     mirror dimensions themselves (44-45/6912) — so the recovery is
        //     the seeding and not a readback that accepts anything.
        //
        // So the null result is a null result and not a blind spot — within
        // the space, which covers one stack rule and no frequency variation,
        // and subject to the control having been run on the flag-off
        // dimensions only. SPEC §11 states that second caveat, and the
        // spread/autocorrelation measurement that bounds it.
        //
        // What IS settled, and lives next door: this dimension's
        // `old_blended_noise` is seeded by the world seed handed straight to
        // the LCG, no fork and no name — `BlendedNoise::legacyFromWorldSeed`,
        // 13824 of 13824 columns over three seeds. That is why the Nether's
        // and the End's terrain SHAPE is nearly in reach while this stays
        // refused: their final densities are old_blended_noise, and it is
        // their named noises this function cannot build.
        //
        // Whether the refusal should be this broad is still open. It fires
        // before `wanted` is even consulted, so a legacy dimension that named
        // no noises at all would be refused too — and nothing here has
        // measured whether that case exists or matters. Narrowing it is a
        // separate question from the derivation, and neither is answered.
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
