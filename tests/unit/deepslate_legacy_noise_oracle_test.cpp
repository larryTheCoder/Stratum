// Stratum — a legacy-source named noise, against deepslate's.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The legacy-seed tables used to carry one candidate under the label
// "deepslate's own derivation": rule 182 of the 900-rule enumeration the
// goldens analyzers share (rule 290 of legacy-seed-analyze.cpp's widened
// one) — base JavaRandom(worldSeed).nextLong(), XOR the first eight bytes of
// MD5("ns:path") big-endian, one further JavaRandom(seed).nextLong() fork,
// driving the LCG, at block offset 0. Nothing recorded how that was known.
//
// These vectors are what deepslate, run as a black box, actually builds for
// a named noise in a legacy_random_source dimension
// (tools/vectors/deepslate_legacy_noise_vectors.mjs), and this file compares
// them with that rule's prediction as tools/analysis/legacy-goldens-surface-
// decoder.hpp computes it — the same candidate code the goldens analyzers and
// vanilla_legacy_goldens_surface_test.cpp score against the server. The
// answer is no: the rule's SEED value is the first long deepslate's named
// generator yields, but deepslate builds the noise from that generator
// itself, unforked, and takes its octaves in cubiomes' `octaveInit` order —
// so no octave rule 182 draws at any of the 300 block offsets is one
// deepslate built. The rule stays in the tables as an anonymous candidate
// (SPEC §11).
//
// What this says about VANILLA is nothing: deepslate is an oracle only where
// it was checked. Its construction is scored against the server separately,
// by name, in vanilla_legacy_goldens_surface_test.cpp, and lands at the null
// there. It is recorded here so that the label is settled by observation
// rather than repeated.
#include "deepslate_legacy_noise_vectors.inc"
#include "legacy-goldens-surface-decoder.hpp"

#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using stratum::noise::PerlinNoise;
using stratum::rng::JavaRandom;

/// The candidate the label was on, and the same rule with no further fork.
constexpr std::size_t kForkedRule = legacy_goldens::kForkedMd5Rule;
constexpr std::size_t kUnforkedRule = legacy_goldens::kUnforkedMd5Rule;

/// How far apart deepslate's values and a construction this file believes
/// is deepslate's may sit. Not zero, for a measured reason: deepslate's
/// LegacyRandom.nextDouble is not java.util.Random's (the primitives case
/// pins by how much), so every Perlin origin it draws is off by up to
/// 256 x 2^-26 and every value by a gradient times that.
constexpr double kAgreement = 1e-5;

[[nodiscard]] double fromBits(std::uint64_t bits) noexcept {
    return std::bit_cast<double>(bits);
}

[[nodiscard]] const DeepslateLegacyNoiseParameters& parametersOf(std::string_view id) {
    const auto found =
        std::ranges::find(kDeepslateLegacyNoises, id, &DeepslateLegacyNoiseParameters::id);
    REQUIRE(found != kDeepslateLegacyNoises.end());
    return *found;
}

[[nodiscard]] std::vector<double> amplitudesOf(const DeepslateLegacyNoiseParameters& noise) {
    return {noise.amplitudes.begin(),
            noise.amplitudes.begin() + static_cast<std::ptrdiff_t>(noise.amplitudeCount)};
}

[[nodiscard]] std::array<std::uint8_t, 256> permutationOf(std::string_view hex) {
    REQUIRE(hex.size() == 512);
    const auto nibble = [](char c) {
        return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4U) | nibble(hex[(2 * i) + 1]));
    }
    return table;
}

/// The LCG seed a rule from the shared enumeration hands its generator.
[[nodiscard]] std::int64_t ruleSeed(std::size_t rule, std::int64_t worldSeed, std::string_view id) {
    return legacy_goldens::seedFor(legacy_goldens::ruleAt(rule), worldSeed, id);
}

/// The first @p count Perlin blocks that rule's generator draws, in order.
[[nodiscard]] std::vector<PerlinNoise> blocksOf(std::size_t rule, std::int64_t worldSeed,
                                                std::string_view id, std::size_t count) {
    return legacy_goldens::blocksFor(legacy_goldens::ruleAt(rule), ruleSeed(rule, worldSeed, id),
                                     count);
}

/// What the enumeration predicts: the sequential stack at block @p offset.
[[nodiscard]] double rulePrediction(std::size_t rule, std::int64_t worldSeed,
                                    const DeepslateLegacyNoiseParameters& noise, std::size_t offset,
                                    double x, double z) {
    const legacy_goldens::Layout layout =
        legacy_goldens::layoutFor(noise.firstOctave, amplitudesOf(noise));
    const std::vector<PerlinNoise> blocks =
        blocksOf(rule, worldSeed, noise.id, offset + layout.blocksPerNoise);
    return legacy_goldens::sampleNormal(layout, blocks, offset, x, 0.0, z);
}

/// The block cubiomes' `octaveInit` order gives declared octave @p slot of
/// stack @p stack (the decoder header's octaveInitBlock, which says how).
[[nodiscard]] std::size_t octaveInitBlock(const DeepslateLegacyNoiseParameters& noise,
                                          std::size_t stack, std::size_t slot) {
    return legacy_goldens::octaveInitBlock(
        noise.firstOctave, static_cast<std::size_t>(noise.amplitudeCount), stack, slot);
}

/// The enumeration's own layout — its persistence schedule and valueFactor —
/// with the blocks in that order. Evaluated by the same sampleNormal, so all
/// that differs from rulePrediction is which generator and which block.
[[nodiscard]] legacy_goldens::Layout octaveInitLayout(const DeepslateLegacyNoiseParameters& noise) {
    return legacy_goldens::octaveInitLayout(noise.firstOctave, amplitudesOf(noise));
}

} // namespace

TEST_CASE("the deepslate legacy noise vectors cover what this file reads",
          "[noise][legacy][oracle]") {
    // Four noises, four seeds, two stacks: 1 + 1 + 3 + 3 slots a stack.
    CHECK(kDeepslateLegacyNoises.size() == 4);
    CHECK(kDeepslateLegacyNamedGenerators.size() == 16);
    CHECK(kDeepslateLegacyOctaves.size() == 64);
    CHECK(kDeepslateLegacyNoiseSamples.size() == 256);
    CHECK(kDeepslateLegacyRandomPrimitives.size() == 8);

    std::size_t present = 0;
    for (const auto& octave : kDeepslateLegacyOctaves) {
        const auto& noise = parametersOf(octave.id);
        REQUIRE(octave.slot < noise.amplitudeCount);
        // deepslate builds an octave exactly where the amplitude is non-zero.
        CHECK(octave.present ==
              legacy_goldens::nonZero(noise.amplitudes[static_cast<std::size_t>(octave.slot)]));
        present += octave.present ? 1U : 0U;
    }
    CHECK(present == 56);

    std::size_t legacy = 0;
    for (const auto& sample : kDeepslateLegacyNoiseSamples) {
        legacy += sample.legacy ? 1U : 0U;
        CHECK(std::isfinite(fromBits(sample.value)));
    }
    CHECK(legacy == 128);
}

TEST_CASE("with the flag off deepslate's named noise is this build's, to the bit",
          "[noise][legacy][oracle]") {
    // The control. Where the derivation is settled against the server, the
    // recorded values are lib's own, so the parameters, the coordinates and
    // the router are wired the way the cases below read them.
    std::size_t exact = 0;
    std::size_t total = 0;
    for (const auto& sample : kDeepslateLegacyNoiseSamples) {
        if (sample.legacy) {
            continue;
        }
        const auto& noise = parametersOf(sample.id);
        const stratum::rng::XoroshiroPositionalFactory factory{sample.seed};
        stratum::rng::Xoroshiro128PlusPlus random = factory.fromHashOf(sample.id);
        const std::vector<double> amplitudes = amplitudesOf(noise);
        const auto normal =
            stratum::noise::NormalNoise::create(random, noise.firstOctave, amplitudes);
        const double ours = normal.sample(sample.x, 0.0, sample.z);
        INFO(sample.id << " seed " << sample.seed << " at " << sample.x << " " << sample.z);
        CHECK(std::bit_cast<std::uint64_t>(ours) == sample.value);
        exact += std::bit_cast<std::uint64_t>(ours) == sample.value ? 1U : 0U;
        ++total;
    }
    CHECK(total == 128);
    CHECK(exact == total);
}

TEST_CASE("deepslate's LegacyRandom is java.util.Random but for nextDouble",
          "[noise][legacy][oracle]") {
    // Measured, because it sets kAgreement: the long and the bounded int are
    // Java's on every seed, the double on none, and never by 2^-26 or more.
    for (const auto& primitive : kDeepslateLegacyRandomPrimitives) {
        INFO("seed " << primitive.seed);
        JavaRandom forLong{primitive.seed};
        CHECK(static_cast<std::uint64_t>(forLong.nextLong()) == primitive.nextLong);
        JavaRandom forInt{primitive.seed};
        CHECK(forInt.nextInt(256) == primitive.nextInt256);
        JavaRandom forDouble{primitive.seed};
        const double java = forDouble.nextDouble();
        const double theirs = fromBits(primitive.nextDouble);
        CHECK(std::bit_cast<std::uint64_t>(java) != primitive.nextDouble);
        CHECK(std::abs(java - theirs) < std::ldexp(1.0, -26));
    }
}

TEST_CASE("deepslate's legacy named generator is JavaRandom(worldSeed).nextLong() xor MD5",
          "[noise][legacy][oracle]") {
    // The two rules differ in the fork alone, and the enumeration says so.
    CHECK(legacy_goldens::describe(legacy_goldens::ruleAt(kForkedRule)) ==
          "lcgLong xor md5FirstBE, forks 1, lcg");
    CHECK(legacy_goldens::describe(legacy_goldens::ruleAt(kUnforkedRule)) ==
          "lcgLong xor md5FirstBE, forks 0, lcg");

    for (const auto& generator : kDeepslateLegacyNamedGenerators) {
        INFO(generator.id << " seed " << generator.seed);
        // The positional base is the world seed's first LCG long, and the
        // other half of its key is unused.
        JavaRandom world{generator.seed};
        CHECK(generator.seedKeyLo == static_cast<std::uint64_t>(world.nextLong()));
        CHECK(generator.seedKeyHi == 0);

        // fromHashOf(id)'s first long IS the forked rule's seed: the fork is
        // that long. Whether the noise is built from a generator seeded with
        // it, or from the generator that yielded it, is what the next two
        // cases tell apart.
        JavaRandom named{ruleSeed(kUnforkedRule, generator.seed, generator.id)};
        CHECK(static_cast<std::uint64_t>(named.nextLong()) == generator.firstLong);
        CHECK(static_cast<std::uint64_t>(ruleSeed(kForkedRule, generator.seed, generator.id)) ==
              generator.firstLong);
    }
}

TEST_CASE("rule 182 does not build deepslate's legacy named noise at any of its block offsets",
          "[noise][legacy][oracle]") {
    // The octaves first. A permutation is drawn by nextInt alone, which
    // deepslate's generator shares with java.util.Random, so it names the
    // generator state an octave came from exactly. Not one of deepslate's 56
    // octaves is any block the forked rule draws over the whole block-offset
    // axis the scans sweep — so this is not a miss at block 0 that some other
    // offset would repair.
    std::size_t found = 0;
    std::size_t octaves = 0;
    for (const auto& generator : kDeepslateLegacyNamedGenerators) {
        const auto& noise = parametersOf(generator.id);
        const legacy_goldens::Layout layout =
            legacy_goldens::layoutFor(noise.firstOctave, amplitudesOf(noise));
        const std::vector<PerlinNoise> blocks =
            blocksOf(kForkedRule, generator.seed, generator.id,
                     legacy_goldens::kBlockOffsets + layout.blocksPerNoise);
        for (const auto& octave : kDeepslateLegacyOctaves) {
            if (octave.seed != generator.seed || octave.id != generator.id || !octave.present) {
                continue;
            }
            ++octaves;
            const std::array<std::uint8_t, 256> table = permutationOf(octave.permutation);
            found += static_cast<std::size_t>(
                std::ranges::count_if(blocks, [&table](const PerlinNoise& block) {
                    return block.permutation() == table;
                }));
        }
    }
    CHECK(octaves == 56);
    CHECK(found == 0);

    // And the values, at block 0 as the label had it: none lands within the
    // bound the construction below meets on every one.
    std::size_t agreed = 0;
    double closest = 1.0e9;
    for (const auto& sample : kDeepslateLegacyNoiseSamples) {
        if (!sample.legacy) {
            continue;
        }
        const double predicted = rulePrediction(kForkedRule, sample.seed, parametersOf(sample.id),
                                                0, sample.x, sample.z);
        const double distance = std::abs(predicted - fromBits(sample.value));
        closest = std::min(closest, distance);
        agreed += distance <= kAgreement ? 1U : 0U;
    }
    INFO("closest miss " << closest);
    CHECK(agreed == 0);
}

TEST_CASE("deepslate's legacy named noise is octaveInit's draw on the unforked generator",
          "[noise][legacy][oracle]") {
    // The positive control for the case above, through the same comparison:
    // with the fork dropped and the blocks taken in octaveInit's order, every
    // permutation is exact, every origin is inside deepslate's nextDouble
    // error, and every value is inside kAgreement. So the miss above is the
    // rule, not this file's reading of the vectors.
    const double originBound = 256.0 * std::ldexp(1.0, -26);
    std::size_t exact = 0;
    std::size_t octaves = 0;
    double worstOrigin = 0.0;
    for (const auto& octave : kDeepslateLegacyOctaves) {
        if (!octave.present) {
            continue;
        }
        ++octaves;
        const auto& noise = parametersOf(octave.id);
        const std::size_t block = octaveInitBlock(noise, static_cast<std::size_t>(octave.stack),
                                                  static_cast<std::size_t>(octave.slot));
        const std::vector<PerlinNoise> blocks =
            blocksOf(kUnforkedRule, octave.seed, octave.id, block + 1);
        const PerlinNoise& ours = blocks[block];
        INFO(octave.id << " seed " << octave.seed << " stack " << octave.stack << " slot "
                       << octave.slot << " block " << block);
        const bool same = ours.permutation() == permutationOf(octave.permutation);
        CHECK(same);
        exact += same ? 1U : 0U;
        for (const auto& [theirs, mine] : {std::array{fromBits(octave.xo), ours.originX()},
                                           std::array{fromBits(octave.yo), ours.originY()},
                                           std::array{fromBits(octave.zo), ours.originZ()}}) {
            worstOrigin = std::max(worstOrigin, std::abs(theirs - mine));
        }
    }
    INFO("worst origin difference " << worstOrigin);
    CHECK(octaves == 56);
    CHECK(exact == 56);
    CHECK(worstOrigin < originBound);

    std::size_t agreed = 0;
    std::size_t legacyAgreed = 0;
    std::size_t legacyChecked = 0;
    double worstValue = 0.0;
    for (const auto& sample : kDeepslateLegacyNoiseSamples) {
        if (!sample.legacy) {
            continue;
        }
        const auto& noise = parametersOf(sample.id);
        const legacy_goldens::Layout layout = octaveInitLayout(noise);
        const double ours = legacy_goldens::sampleNormal(
            layout, blocksOf(kUnforkedRule, sample.seed, sample.id, layout.blocksPerNoise), 0,
            sample.x, 0.0, sample.z);
        const double theirs = fromBits(sample.value);
        worstValue = std::max(worstValue, std::abs(ours - theirs));
        agreed += std::abs(ours - theirs) <= kAgreement ? 1U : 0U;

        // Where every amplitude is one, that order is lib's own legacy
        // construction, the one the Nether's climate is built with.
        const std::vector<double> amplitudes = amplitudesOf(noise);
        if (std::ranges::none_of(amplitudes,
                                 [](double a) { return legacy_goldens::nonZero(a - 1.0); })) {
            JavaRandom random{ruleSeed(kUnforkedRule, sample.seed, sample.id)};
            const auto legacy = stratum::noise::NormalNoise::createLegacy(random, noise.firstOctave,
                                                                          noise.amplitudeCount);
            legacyAgreed +=
                std::abs(legacy.sample(sample.x, 0.0, sample.z) - theirs) <= kAgreement ? 1U : 0U;
            ++legacyChecked;
        }
    }
    INFO("worst value difference " << worstValue);
    CHECK(agreed == 128);
    CHECK(legacyChecked == 96);
    CHECK(legacyAgreed == legacyChecked);
}

TEST_CASE("deepslate's own octaves give its values through this file's evaluator",
          "[noise][legacy][oracle]") {
    // The other half of the control: deepslate's recorded octaves, rebuilt as
    // lib PerlinNoise and summed by the enumeration's sampleNormal in
    // octaveInit's layout, give its recorded values to the bit. So the
    // difference the case above allows for is the origins alone — deepslate's
    // nextDouble — and not the persistence, the valueFactor or the order of
    // the sum.
    std::size_t exact = 0;
    std::size_t total = 0;
    double worst = 0.0;
    for (const auto& sample : kDeepslateLegacyNoiseSamples) {
        if (!sample.legacy) {
            continue;
        }
        const auto& noise = parametersOf(sample.id);
        const legacy_goldens::Layout layout = octaveInitLayout(noise);
        std::vector<PerlinNoise> blocks(
            layout.blocksPerNoise, PerlinNoise{0.0, 0.0, 0.0, std::array<std::uint8_t, 256>{}});
        for (const auto& octave : kDeepslateLegacyOctaves) {
            if (octave.seed != sample.seed || octave.id != sample.id || !octave.present) {
                continue;
            }
            blocks[octaveInitBlock(noise, static_cast<std::size_t>(octave.stack),
                                   static_cast<std::size_t>(octave.slot))] =
                PerlinNoise{fromBits(octave.xo), fromBits(octave.yo), fromBits(octave.zo),
                            permutationOf(octave.permutation)};
        }
        const double ours =
            legacy_goldens::sampleNormal(layout, blocks, 0, sample.x, 0.0, sample.z);
        worst = std::max(worst, std::abs(ours - fromBits(sample.value)));
        exact += std::bit_cast<std::uint64_t>(ours) == sample.value ? 1U : 0U;
        ++total;
    }
    INFO("exact " << exact << " of " << total << ", worst " << worst);
    CHECK(total == 128);
    CHECK(exact == total);
}
