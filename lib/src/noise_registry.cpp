// Stratum — the noises a density function graph names.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

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

#include <cstdint>
#include <map>
#include <set>
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

std::string legacyConstructRefusal(const std::string_view construct,
                                   const std::string_view detail) {
    // Same shape as the noise refusal above and deliberately so: it names the
    // construct, says what it needs, and says what this build will not do
    // instead (SPEC §8, §11).
    return "this dimension declares legacy_random_source and needs '" + std::string(construct) +
           "', which draws a random from that source rather than naming a worldgen/noise — " +
           std::string(detail) +
           ". How the Java LCG is seeded for it is not settled here, and this build will not "
           "substitute the modern Xoroshiro derivation: for vertical_gradient that substitution "
           "is measured to agree with vanilla at chance (SPEC §11)";
}

NoiseRegistry NoiseRegistry::create(const data::Pack& pack,
                                    std::span<const data::ResourceLocation> wanted,
                                    std::int64_t worldSeed, RandomSource source) {
    std::map<data::ResourceLocation, NoiseParameters> parameters;
    if (source == RandomSource::Legacy && !wanted.empty()) {
        // No lookup at all under Legacy. The three climate noises it can
        // build ignore the pack's parameters (see the overload below), and
        // every other name is refused for the reason that actually blocks it
        // — the seeding — rather than for a missing entry it could never have
        // seeded anyway. A legacy dimension that names nothing falls through
        // to the ordinary path and builds an empty registry, which is the
        // correct answer rather than a lenient one.
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
    // The three climate noises are settled under Legacy (above); only the
    // rest of `wanted` still reaches the refusal below, and only the rest is
    // named in it.
    std::vector<data::ResourceLocation> unresolved;
    if (source == RandomSource::Legacy) {
        for (const data::ResourceLocation& id : wanted) {
            if (!isLegacyClimateNoise(id)) {
                unresolved.push_back(id);
            }
        }
    }
    if (source == RandomSource::Legacy && !unresolved.empty()) {
        // NARROWED, and the narrowing is a measurement rather than a guess.
        // This used to fire on the source alone, before `wanted` was
        // consulted at all, so it refused a legacy dimension that named NO
        // noise — a dimension with nothing to derive a seed for. Its own note
        // used to end by saying nobody had measured whether that case
        // existed. It does, and it is most of what was blocked. Walking the
        // pinned pack per router entry, splines and shared density-function
        // references followed:
        //
        //   end.json               0 named noises, router AND surface rule
        //   nether.json            3 in the router — `minecraft:temperature`,
        //                          `minecraft:vegetation` and
        //                          `minecraft:offset`, which arrives through
        //                          the shared shift_x/shift_z — and 8 in the
        //                          surface rule; 0 in all THIRTEEN other
        //                          router entries, final_density included
        //   caves.json             the same 3, and 9 in the surface rule
        //   floating_islands.json  the same 3, and 9 in the surface rule
        //
        // (`offset` is easy to miss and the tests say why: `shift_a` spells
        // its noise field "argument", and it is reached through the
        // referenced `minecraft:shift_x`/`shift_z`, in a different file. The
        // surface counts are what a rule tree actually needs to run, which is
        // more than the identifiers written in it — see
        // surface::requiredNoises.)
        //
        // Two independent walks agree on the table — a hand-built
        // type->field walk and Graph::reachableFrom — see
        // tests/conformance/vanilla_legacy_named_noises_test.cpp, plus
        // tools/analysis/legacy-named-noise-reach.py as a third route.
        //
        // So an empty `wanted` is not "refuse quietly less": there is no
        // identifier to turn into a seed, so nothing is approximated and
        // nothing is at risk. A non-empty one is refused exactly as before.
        //
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
        // WHAT THE NARROWING ACTUALLY REACHES, so the next reader does not
        // over-read it. It unblocks a legacy dimension's terrain only where
        // that terrain names no noise, which is every legacy dimension — and
        // for the End, which names none anywhere, it unblocks the surface
        // rules too. The climate noises are now derived as well (above), so
        // what the other three still need from this function is their
        // SURFACE RULES' noises alone — the Nether's eight, nine each for
        // caves and floating islands — and CompiledDimension::compile still
        // refuses them for exactly those. The End compiles when paired with
        // an existing biome list; its own `minecraft:the_end` biome source is
        // not implemented. See SPEC §11.
        //
        // The names go in the message. Which noises a legacy dimension still
        // cannot have is the actionable part of this refusal now that it is
        // no longer all-or-nothing: "temperature, vegetation" says the
        // terrain is fine and the climate is not, where the old wording said
        // only that the dimension was out (SPEC §8 — unsupported input fails
        // loudly, naming what it could not do).
        // Deduplicated: callers assemble `wanted` by concatenating a density
        // graph's names with a surface rule graph's, and `minecraft:surface`
        // legitimately appears in both. A message that listed it twice would
        // read as two different problems.
        //
        // NARROWED AGAIN, to what is still unsolved: the three climate noises
        // are derived (the block above this function) and are neither built
        // nor named here when anything else is refused, because naming them
        // would send the reader after a problem that no longer exists.
        const std::set<data::ResourceLocation> unique{unresolved.begin(), unresolved.end()};
        std::string names;
        for (const data::ResourceLocation& id : unique) {
            if (!names.empty()) {
                names += ", ";
            }
            names += id.toString();
        }
        throw NoiseError(
            "this dimension declares legacy_random_source and names " +
            std::to_string(unique.size()) + " noise(s) — " + names +
            " — and how a noise's name becomes a seed under the Java LCG is not settled here, "
            "so they cannot be built (only minecraft:temperature, minecraft:vegetation and "
            "minecraft:offset are). This build will not substitute the modern derivation "
            "(SPEC §11)");
    }

    NoiseRegistry registry;
    registry.worldSeed_ = worldSeed;
    registry.source_ = source;

    if (source == RandomSource::Legacy) {
        for (const data::ResourceLocation& id : wanted) {
            if (!registry.noises_.contains(id)) {
                registry.noises_.emplace(id, legacyClimateNoise(id, worldSeed));
            }
        }
        return registry;
    }

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
