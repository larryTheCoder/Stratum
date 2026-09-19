// Stratum — the legacy Nether's surface noises, read out of vanilla's regions.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS PINS. The eight golden Nether regions are ~2 million columns of
// the real server's output, every one of them painted by the real legacy
// seeding of the six noises SPEC §11 says this build cannot derive. A
// `noise_threshold` condition is a sign test, so each placed block is, read
// backwards, one bit about which side of a threshold a noise was on. This
// case is that readback, its control, and what it excludes.
//
// EIGHT REGIONS, SIX WORLDS. java.util.Random scrambles a seed as
// `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, discarding bit 63, so
// 0 == Long.MIN_VALUE and -1 == Long.MAX_VALUE. Every denominator below is
// over six independent worlds however many region files were read.
//
// THE DECODER IS NOT HAND-READ, and that is the whole design. It walks the
// resolved `surface::RuleGraph` at each position, evaluating every condition
// it can evaluate from the golden region and BRANCHING on every one it
// cannot — the noise thresholds it is reading, a `vertical_gradient` inside
// its own band, and the column's surface depth, which is itself a named noise
// under a legacy source and so is ENUMERATED over the range
// `minecraft:surface`'s own parameters bound it to. A condition is observed
// only where every assignment that reproduces the golden block agrees on it.
// See tools/analysis/legacy-goldens-surface-decoder.hpp, which this case
// INCLUDES rather than reimplements.
//
// THE CONTROL COMES FIRST AND IT IS THE POINT. The same decoder is run over
// the golden OVERWORLD regions, whose surface-rule noises are modern-seeded
// and which this build reproduces exactly. Three arms:
//
//   replay    the library's own surface::Executor, on the Context this
//             decoder reconstructed out of the post-rule region, reproduces
//             the golden block. This is what says the reconstruction — the
//             stone-depth runs, the water height, the visited positions — is
//             right, independently of the symbolic walk.
//             262406910 / 262581476 = 99.9335% at stride 1 over eight
//             regions; the rest are where a rule changed a position's
//             CATEGORY (the overworld tree places `minecraft:water` and
//             `minecraft:air`) and a post-rule region cannot say what was
//             underneath. Only the positions it reproduces are scored, and
//             on those the tree explains EVERY golden block: 0 unexplained.
//   recovery  the decoded bits against the TRUE modern noise.
//             30104 of 30104 — 100.0000%.
//   negative  the same bits against the same noises built at worldSeed + 1.
//             78.97%, BELOW the 86.37% a constant answer already scores:
//             the wrong seeding is indistinguishable from answering with a
//             constant.
//
// A readback whose control did not recover a known-correct seeding would be
// measuring its own decoder, and this file would rather skip than assert a
// Nether number without it.
//
// WHAT THE NETHER READBACK FINDS. Nothing survives. Over the whole committed
// 270,000-candidate space (900 seed rules x 300 block offsets, the space
// tools/analysis/legacy-seed-analyze.cpp enumerates, restated here so the two
// tools can disagree), scored against the 545395 columns the readback decides
// for `minecraft:nether_state_selector` at stride 1, the BEST candidate
// reaches 51.36% — against a trivial predictor of 50.63% and a null MEASURED
// on those same columns of 50.02% +/- 0.61, whose own observed maximum is
// 52.21%. Nothing in the space clears the null's own maximum. The same on
// `minecraft:netherrack`, 688833 columns: best 96.02%, null 95.66% +/- 0.32
// max 96.66%, trivial predictor 97.77%. The null is measured rather than
// computed because it has to be: the decoded columns are spatially clustered
// and a candidate noise is spatially smooth, so a wrong rule's agreement has
// an effective sample size of patches, not columns, and sqrt(n) would call
// every leader an impossible outlier. SPEC §11 carries all five noises.
//
// AND THE SCAN CAN FIND A CORRECT RULE. `--plant` replaces the server's bits
// with the bits a chosen candidate would have produced and runs the identical
// scan: it comes back rank 1 at 100.0000% on every decoded column, against a
// runner-up at 66%. So "no survivor" is a measurement of the space, not a
// property of the apparatus.
//
// THE IDENTITY TEST is the one thing here that is not a null result.
// `minecraft:soul_sand_layer` and `minecraft:gravel_layer` have byte-identical
// parameters — firstOctave -8, amplitudes [1,1,1,1,0,0,0,0,0.01333...] — and
// differ only by name, and the Nether's `nether_wastes` branch reads both at
// -0.012 in a way that decides BOTH at the same column. If the name did not
// enter the seed they would be one field, and the pair (soul below, gravel at
// or above) would be impossible. The joint table at stride 1 is
// (F,F) 1800, (F,T) 1398, (T,F) 0, (T,T) 6 — so the impossible cell is
// 1398 of 3204 columns, 43.63%. So something about the identifier — its hash,
// or the order the noises are built in, which this cannot separate — reaches
// the seed.
//
// WHAT THIS EXCLUDES, AND WHAT IT DOES NOT.
//
//   * It excludes the 270,000 candidates of that space for the five noises
//     whose decoded bit is two-sided — `nether_state_selector`,
//     `netherrack`, `patch`, `soul_sand_layer`, `gravel_layer` — on this
//     oracle. One noise at a time against one threshold: nothing here tests
//     whether the six share a seeding RULE, so this is five independent
//     refutations rather than one of a joint rule.
//   * It does NOT widen the space. The same two gaps stand: one stack rule
//     (sequential Perlin blocks from a single generator), no per-octave
//     salting, no frequency rule but the declared firstOctave, and no
//     discarded-LCG-STEP offset of the kind `minecraft:end_islands` uses.
//   * `minecraft:nether_wart` is observed on 674596 columns and carries NO
//     information: its threshold is 1.17 and not one of them reaches it, so
//     its bit is constant and its null equals its signal. It is reported and
//     not scanned.
//   * The Nether run has no replay arm — its tree cannot be compiled under a
//     legacy source at all — so its reconstruction is bounded only by the
//     overworld's 99.9335% and by its own two reported rates: 262795 of
//     176537818 positions (0.1489%) the tree cannot explain, and columns
//     where two positions decode one condition BOTH ways, which cannot
//     honestly happen and are dropped whole (8514 of 238576 on the
//     worst-affected condition, 0 on the best). Vanilla's `hole` branch
//     replacing a solid block with LAVA below y = 32 is the named cause: a
//     post-rule region cannot undo it, and the stone-depth run under it is
//     then one short.
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Without
// them this skips.
#include "legacy-goldens-surface-decoder.hpp"

#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

using legacy_goldens::ControlArm;
using legacy_goldens::Decoder;
using legacy_goldens::Field;
using legacy_goldens::kGoldenSeeds;
using legacy_goldens::Known;
using legacy_goldens::percent;
using legacy_goldens::Replay;
using legacy_goldens::Walk;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::NoiseSettings;
using stratum::surface::RuleGraph;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// Every chunk is 256 columns and this case walks two dimensions over eight
/// regions, so the default takes every eighth chunk in both axes: 16 of each
/// region's 1024. STRATUM_GOLDEN_SURFACE_STRIDE=1 runs the whole thing, which
/// is what the numbers quoted in SPEC §11 are measured at.
[[nodiscard]] std::int32_t stride() {
    if (const char* override = std::getenv("STRATUM_GOLDEN_SURFACE_STRIDE")) {
        const int value = std::atoi(override);
        if (value > 0 && value <= 32) {
            return value;
        }
    }
    return 8;
}

} // namespace

TEST_CASE("the golden-region surface readback recovers a known-correct modern seeding",
          "[conformance][legacy][surface][goldens][control]") {
    const std::filesystem::path tree = legacy_goldens::findWorldgenTree(fixtures());
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }
    const Pack pack = Pack::open(tree);
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const auto id = ResourceLocation::parse("minecraft:overworld");
    const NoiseSettings& overworld = loaded.settings.at(id);
    REQUIRE_FALSE(overworld.legacyRandomSource);

    const RuleGraph graph = RuleGraph::resolve(overworld.surfaceRule, id);
    const Decoder decoder{graph, overworld};

    std::vector<ResourceLocation> wanted = stratum::surface::requiredNoises(graph);
    for (const ResourceLocation& name : loaded.graph.referencedNoises()) {
        wanted.push_back(name);
    }
    std::ranges::sort(wanted);
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    const stratum::biome::TemperatureTable temperatures =
        stratum::biome::TemperatureTable::fromPack(pack);

    std::map<std::string, ControlArm> recovery;
    std::map<std::string, ControlArm> negative;
    std::size_t regions = 0;
    std::size_t replayed = 0;
    std::size_t replayAgreed = 0;
    std::size_t unexplained = 0;

    for (const std::int64_t seed : kGoldenSeeds) {
        const std::filesystem::path region =
            legacy_goldens::regionOf(fixtures(), seed, "overworld");
        if (region.empty()) {
            continue;
        }
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, wanted, seed, stratum::density::RandomSource::Xoroshiro);
        const auto wrong = stratum::density::NoiseRegistry::create(
            pack, wanted, seed + 1, stratum::density::RandomSource::Xoroshiro);
        const stratum::surface::Executor executor = stratum::surface::Executor::compile(
            graph, seed, overworld.geometry, &noises, overworld.seaLevel);
        const stratum::noise::NormalNoise& secondary =
            noises.get(ResourceLocation::parse("minecraft:surface_secondary"));
        legacy_goldens::PreliminarySurface preliminary{loaded.graph, noises, overworld};
        const Replay replay{.executor = &executor, .temperatures = &temperatures, .gate = true};

        Walk walk;
        walk.tallies.assign(graph.conditionCount(), {});
        walk.fields.assign(graph.conditionCount(), {});
        walk.keepFields = true;
        legacy_goldens::walkRegion(
            region, overworld, decoder, stride(), true, replay,
            [&](std::int32_t x, std::int32_t z) {
                Known known;
                known.surfaceDepth = executor.surfaceDepth(x, z);
                known.surfaceSecondary =
                    secondary.sample(static_cast<double>(x), 0.0, static_cast<double>(z));
                known.preliminarySurface = preliminary.at(x, z);
                return known;
            },
            walk);
        legacy_goldens::scoreAgainstRegistry(graph, decoder, noises, walk, recovery);
        legacy_goldens::scoreAgainstRegistry(graph, decoder, wrong, walk, negative);
        replayed += walk.stats.replayed;
        replayAgreed += walk.stats.replayAgreed;
        unexplained += walk.stats.unexplained;
        ++regions;
    }

    if (regions == 0) {
        SKIP("no golden overworld regions under " << STRATUM_FIXTURES_DIR
                                                  << " — generate them with tools/fetch-vanilla");
    }

    std::size_t bits = 0;
    std::size_t agreed = 0;
    std::size_t majority = 0;
    std::size_t wrongAgreed = 0;
    for (const auto& [name, arm] : recovery) {
        bits += arm.bits;
        agreed += arm.agreed;
        majority += arm.majority;
        wrongAgreed += negative[name].agreed;
    }
    INFO("regions " << regions << ", replay " << replayAgreed << "/" << replayed << ", bits "
                    << bits << ", recovery " << agreed << ", majority " << majority << ", seed+1 "
                    << wrongAgreed);

    // The reconstruction is checked independently of the decoder.
    CHECK(percent(replayAgreed, replayed) > 99.0);
    // On a gated position the tree explains the block by construction; a
    // position it does not is a decoder bug, not a tolerance.
    CHECK(unexplained == 0);
    // Enough bits that 100% is a statement rather than an accident.
    REQUIRE(bits > 200);
    // THE CONTROL. A known-correct seeding, through this decoder, on the
    // server's own regions.
    CHECK(agreed == bits);
    // AND THE NEGATIVE ARM. The wrong seeding must sit at the trivial
    // predictor — these bits are lopsided, so "78%" only means anything
    // beside the 78% a constant answer already scores.
    CHECK(percent(wrongAgreed, bits) <= percent(majority, bits) + 1.0);
}

TEST_CASE("two Nether surface noises with identical parameters read different fields",
          "[conformance][legacy][surface][goldens][identity]") {
    const std::filesystem::path tree = legacy_goldens::findWorldgenTree(fixtures());
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }
    const Pack pack = Pack::open(tree);
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const auto id = ResourceLocation::parse("minecraft:nether");
    const NoiseSettings& nether = loaded.settings.at(id);
    REQUIRE(nether.legacyRandomSource);

    // The premise, read off the pack rather than remembered: the two noises
    // really are byte-identical apart from their names.
    const auto soulParameters = stratum::density::NoiseParameters::fromJson(
        pack.find(stratum::data::Registry::Noise,
                  ResourceLocation::parse("minecraft:soul_sand_layer"))
            ->json,
        ResourceLocation::parse("minecraft:soul_sand_layer"));
    const auto gravelParameters = stratum::density::NoiseParameters::fromJson(
        pack.find(stratum::data::Registry::Noise, ResourceLocation::parse("minecraft:gravel_layer"))
            ->json,
        ResourceLocation::parse("minecraft:gravel_layer"));
    REQUIRE(soulParameters.firstOctave == gravelParameters.firstOctave);
    REQUIRE(soulParameters.amplitudes == gravelParameters.amplitudes);

    const RuleGraph graph = RuleGraph::resolve(nether.surfaceRule, id);
    const Decoder decoder{graph, nether};

    stratum::surface::ConditionIndex soul = 0;
    stratum::surface::ConditionIndex gravel = 0;
    bool haveSoul = false;
    bool haveGravel = false;
    for (stratum::surface::ConditionIndex i = 0; i < graph.conditionCount(); ++i) {
        if (decoder.noiseOf(i) < 0) {
            continue;
        }
        const std::string name = graph.condition(i).noise->toString();
        if (name == "minecraft:soul_sand_layer") {
            soul = i;
            haveSoul = true;
        } else if (name == "minecraft:gravel_layer") {
            gravel = i;
            haveGravel = true;
        }
    }
    REQUIRE(haveSoul);
    REQUIRE(haveGravel);
    // Both at the same threshold, which is what makes one field predict
    // agreement rather than merely correlation.
    REQUIRE_FALSE(
        std::abs(graph.condition(soul).minThreshold - graph.condition(gravel).minThreshold) > 0.0);

    std::size_t regions = 0;
    std::size_t positions = 0;
    std::size_t unexplained = 0;
    std::size_t both = 0;
    std::size_t impossibleUnderIdentity = 0;
    std::size_t netherStateSelectorColumns = 0;

    for (const std::int64_t seed : kGoldenSeeds) {
        const std::filesystem::path region = legacy_goldens::regionOf(fixtures(), seed, "nether");
        if (region.empty()) {
            continue;
        }
        Walk walk;
        walk.tallies.assign(graph.conditionCount(), {});
        walk.fields.assign(graph.conditionCount(), {});
        walk.keepFields = true;
        legacy_goldens::walkRegion(
            region, nether, decoder, stride(), false, Replay{},
            [](std::int32_t, std::int32_t) { return Known{}; }, walk);
        for (std::size_t slot = 0; slot < legacy_goldens::kColumnsPerRegion; ++slot) {
            const std::int8_t a = walk.fields[soul].bits[slot];
            const std::int8_t b = walk.fields[gravel].bits[slot];
            if (a < 0 || b < 0) {
                continue;
            }
            ++both;
            if (a != b) {
                ++impossibleUnderIdentity;
            }
        }
        for (stratum::surface::ConditionIndex i = 0; i < graph.conditionCount(); ++i) {
            if (decoder.noiseOf(i) < 0 ||
                graph.condition(i).noise->toString() != "minecraft:nether_state_selector") {
                continue;
            }
            netherStateSelectorColumns += walk.tallies[i].columns;
        }
        positions += walk.stats.positions;
        unexplained += walk.stats.unexplained;
        ++regions;
    }

    if (regions == 0) {
        SKIP("no golden nether regions under " << STRATUM_FIXTURES_DIR
                                               << " — generate them with tools/fetch-vanilla");
    }

    INFO("regions " << regions << ", positions " << positions << ", unexplained " << unexplained
                    << ", columns deciding both " << both << ", disagreeing "
                    << impossibleUnderIdentity);

    // The readback has to reach the Nether's surface at all.
    REQUIRE(positions > 100000);
    // The decoder explains all but a small tail. That tail is not waved at:
    // vanilla's `hole` branch replaces a solid block with LAVA below y = 32,
    // which a post-rule region cannot undo, and those columns' stone-depth
    // runs are then one short.
    CHECK(percent(unexplained, positions) < 1.0);

    // THE TEST. Under one field the two bits could never differ.
    REQUIRE(both > 50);
    CHECK(impossibleUnderIdentity > 0);
    CHECK(percent(impossibleUnderIdentity, both) > 10.0);

    // And the readback the scan runs on is not a handful of columns.
    CHECK(netherStateSelectorColumns > 10000);
}

TEST_CASE("no candidate legacy seeding reproduces the Nether's decoded surface noise",
          "[conformance][legacy][surface][goldens][scan]") {
    const std::filesystem::path tree = legacy_goldens::findWorldgenTree(fixtures());
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }
    const Pack pack = Pack::open(tree);
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const auto id = ResourceLocation::parse("minecraft:nether");
    const NoiseSettings& nether = loaded.settings.at(id);
    const RuleGraph graph = RuleGraph::resolve(nether.surfaceRule, id);
    const Decoder decoder{graph, nether};

    const std::string target = "minecraft:nether_state_selector";
    stratum::surface::ConditionIndex pick = 0;
    bool havePick = false;
    for (stratum::surface::ConditionIndex i = 0; i < graph.conditionCount(); ++i) {
        if (decoder.noiseOf(i) >= 0 && graph.condition(i).noise->toString() == target) {
            pick = i;
            havePick = true;
            break;
        }
    }
    REQUIRE(havePick);
    const double threshold = graph.condition(pick).minThreshold;
    const double maxThreshold = graph.condition(pick).maxThreshold;

    struct Column {
        std::int64_t seed;
        std::int32_t x;
        std::int32_t z;
        std::int8_t bit;
    };

    std::vector<Column> columns;
    std::vector<std::int64_t> seeds;

    for (const std::int64_t seed : kGoldenSeeds) {
        const std::filesystem::path region = legacy_goldens::regionOf(fixtures(), seed, "nether");
        if (region.empty()) {
            continue;
        }
        Walk walk;
        walk.tallies.assign(graph.conditionCount(), {});
        walk.fields.assign(graph.conditionCount(), {});
        walk.keepFields = true;
        legacy_goldens::walkRegion(
            region, nether, decoder, stride(), false, Replay{},
            [](std::int32_t, std::int32_t) { return Known{}; }, walk);
        std::vector<std::int8_t> merged(legacy_goldens::kColumnsPerRegion, -1);
        for (stratum::surface::ConditionIndex i = 0; i < graph.conditionCount(); ++i) {
            if (decoder.noiseOf(i) < 0 || graph.condition(i).noise->toString() != target) {
                continue;
            }
            for (std::size_t slot = 0; slot < merged.size(); ++slot) {
                const std::int8_t bit = walk.fields[i].bits[slot];
                if (bit < 0) {
                    continue;
                }
                if (merged[slot] >= 0 && merged[slot] != bit) {
                    merged[slot] = Field::kPoisoned;
                } else if (merged[slot] != Field::kPoisoned) {
                    merged[slot] = bit;
                }
            }
        }
        for (std::size_t slot = 0; slot < merged.size(); ++slot) {
            if (merged[slot] < 0) {
                continue;
            }
            columns.push_back({.seed = seed,
                               .x = static_cast<std::int32_t>(slot % 512U),
                               .z = static_cast<std::int32_t>(slot / 512U),
                               .bit = merged[slot]});
        }
        seeds.push_back(seed);
    }

    if (seeds.empty()) {
        SKIP("no golden nether regions under " << STRATUM_FIXTURES_DIR
                                               << " — generate them with tools/fetch-vanilla");
    }
    REQUIRE(columns.size() > 5000);

    const auto parameters = stratum::density::NoiseParameters::fromJson(
        pack.find(stratum::data::Registry::Noise, ResourceLocation::parse(target))->json,
        ResourceLocation::parse(target));
    const legacy_goldens::Layout layout =
        legacy_goldens::layoutFor(parameters.firstOctave, parameters.amplitudes);

    const auto score = [&](std::size_t ruleIndex, std::size_t offset) {
        const legacy_goldens::SeedRule rule = legacy_goldens::ruleAt(ruleIndex);
        std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> blocks;
        for (const std::int64_t seed : seeds) {
            blocks.emplace(
                seed, legacy_goldens::blocksFor(rule, legacy_goldens::seedFor(rule, seed, target),
                                                offset + layout.blocksPerNoise));
        }
        std::size_t agreed = 0;
        for (const Column& column : columns) {
            const double value = legacy_goldens::sampleNormal(layout, blocks.at(column.seed),
                                                              offset, static_cast<double>(column.x),
                                                              0.0, static_cast<double>(column.z));
            const bool holds = threshold <= value && value <= maxThreshold;
            if (holds == (column.bit == 1)) {
                ++agreed;
            }
        }
        return agreed;
    };

    // The bit has to be two-sided, or agreement means nothing.
    const auto trueBits = static_cast<std::size_t>(
        std::count_if(columns.begin(), columns.end(), [](const Column& c) { return c.bit == 1; }));
    const double majority = percent(std::max(trueBits, columns.size() - trueBits), columns.size());
    INFO("columns " << columns.size() << ", true " << trueBits << ", majority null " << majority);
    CHECK(majority < 60.0);

    // deepslate's own derivation, BY NAME rather than left to be inferred
    // from a list it is absent from: rule 182, block 0.
    const std::size_t deepslate = score(legacy_goldens::kDeepslateRule, 0);
    INFO("deepslate rule " << legacy_goldens::kDeepslateRule << " scores " << deepslate << "/"
                           << columns.size());
    CHECK(percent(deepslate, columns.size()) < majority);

    // A BOUNDED SWEEP, not the whole space: all 900 seed rules at block
    // offsets 0..1, which is 1800 of the 270000 candidates. The full sweep
    // lives in tools/analysis/stratum_legacy_goldens_surface_analyze and its
    // headline is quoted in SPEC §11; what this pins is that nothing in the
    // part a test can afford comes anywhere near a correct rule.
    std::size_t peak = 0;
    std::size_t peakRule = 0;
    for (std::size_t ruleIndex = 0; ruleIndex < legacy_goldens::kSeedRules; ++ruleIndex) {
        for (std::size_t offset = 0; offset < 2; ++offset) {
            const std::size_t agreed = score(ruleIndex, offset);
            if (agreed > peak) {
                peak = agreed;
                peakRule = ruleIndex;
            }
        }
    }
    INFO("best of 1800 candidates: rule " << peakRule << " at " << peak << "/" << columns.size());
    CHECK(percent(peak, columns.size()) < 80.0);

    // AND THE CALIBRATION, without which the line above is not a measurement.
    // Plant a candidate's own bits in place of the server's and rescore it:
    // a correct rule is recovered EXACTLY, so this readback would not miss
    // one.
    constexpr std::size_t kPlantRule = 417;
    constexpr std::size_t kPlantBlock = 1;
    {
        const legacy_goldens::SeedRule rule = legacy_goldens::ruleAt(kPlantRule);
        std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> blocks;
        for (const std::int64_t seed : seeds) {
            blocks.emplace(
                seed, legacy_goldens::blocksFor(rule, legacy_goldens::seedFor(rule, seed, target),
                                                kPlantBlock + layout.blocksPerNoise));
        }
        for (Column& column : columns) {
            const double value = legacy_goldens::sampleNormal(
                layout, blocks.at(column.seed), kPlantBlock, static_cast<double>(column.x), 0.0,
                static_cast<double>(column.z));
            column.bit =
                static_cast<std::int8_t>(threshold <= value && value <= maxThreshold ? 1 : 0);
        }
    }
    CHECK(score(kPlantRule, kPlantBlock) == columns.size());
}
