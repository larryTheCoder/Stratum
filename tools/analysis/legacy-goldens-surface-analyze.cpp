// Stratum — the legacy Nether's surface noises, read out of vanilla's regions.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/analysis/legacy-seed-analyze.cpp asks how a NAMED noise's identifier
// becomes a Java LCG seed by inverting a synthetic probe dimension's terrain
// height. This asks the same question of a different oracle, one that was
// sitting unused: the eight golden Nether regions — SIX INDEPENDENT WORLDS,
// since java.util.Random discards bit 63 and 0 == Long.MIN_VALUE,
// -1 == Long.MAX_VALUE — whose every column was painted by the real legacy
// seeding of exactly the six surface noises in question.
//
// A `noise_threshold` condition is a sign test. So a block vanilla placed is,
// read backwards, one bit about which side of a threshold a noise was on —
// but only for the conditions the tree actually consulted before placing it.
// tools/analysis/legacy-goldens-surface-decoder.hpp does that inversion by
// walking the resolved rule graph and branching on everything it cannot
// evaluate; this file drives it, controls it, and scores candidates against
// it.
//
// THE ORDER MATTERS AND IT IS NOT NEGOTIABLE. `--control` runs the identical
// decoder over the eight golden OVERWORLD regions, whose surface-rule noises
// are MODERN-seeded and which this build already reproduces exactly. If the
// decoder is wrong — a misread `stone_depth`, a stone-depth run reconstructed
// with the wrong convention, a missed masking branch — the overworld run says
// so, because there the right answer is known. A `--scan` result without a
// passing `--control` beside it is not a measurement, it is a number.
//
//   modes
//     --control        overworld goldens: a replay arm on the reconstructed
//                      Context, a recovery arm on the decoded bits, and a
//                      wrong-seed negative arm, all through the same decoder
//     --decode         nether goldens: which (noise, threshold) pairs the
//                      placed block decides, with denominators, how many
//                      positions the tree consulted them at without deciding
//                      them, and the identity test
//     --identity       the identity test alone
//     --scan <noise>   nether goldens: the committed 270,000-candidate space
//                      scored against one noise's decoded bits, then the
//                      leaders rescored on every decoded column, then the
//                      null measured on those same columns
//     --scan-all       the same for every noise the readback observes, off
//                      ONE decode
//
//   modifiers
//     --stride N       take every Nth chunk of each region (default 1)
//     --plant R B      replace the server's bits with candidate (R, B)'s own
//                      and rescan: the calibration arm
//     --census         print the per-condition census for --control too
//     --trust-steep    read `steep` off the golden's heights instead of
//                      branching on it
//     --symbolic-water branch on every `water` condition instead of reading
//                      the golden's fluid blocks
//     --enumerate      enumerate the overworld's surface depth too, exactly
//                      as the Nether run must
//     --no-replay-gate score every position, not only those the library's own
//                      Executor reproduces
//
//   cmake --build --preset dev --target stratum_legacy_goldens_surface_analyze
//   A=build/dev/tools/analysis/stratum_legacy_goldens_surface_analyze
//   $A .fixtures/1.21.11 --control --trust-steep
//   $A .fixtures/1.21.11 --decode
//   $A .fixtures/1.21.11 --scan minecraft:nether_state_selector
//   $A .fixtures/1.21.11 --scan minecraft:nether_state_selector --plant 417 23
//
// The fixtures are Mojang-derived and never committed (SPEC §12); without
// them every mode reports what is missing and exits 77, CTest's skip code.
#include "legacy-goldens-surface-decoder.hpp"

#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using legacy_goldens::ConditionTally;
using legacy_goldens::ControlArm;
using legacy_goldens::Decoder;
using legacy_goldens::describeCondition;
using legacy_goldens::Field;
using legacy_goldens::kGoldenSeeds;
using legacy_goldens::Known;
using legacy_goldens::Observed;
using legacy_goldens::percent;
using legacy_goldens::PreliminarySurface;
using legacy_goldens::Replay;
using legacy_goldens::scoreAgainstRegistry;
using legacy_goldens::Walk;
using legacy_goldens::walkRegion;
using legacy_goldens::WalkStats;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::NoiseSettings;
using stratum::surface::RuleGraph;

/// One region is 32x32 chunks; this takes every `stride`-th one in both axes.
/// Stride 1 is the whole region — 1024 chunks, 262144 columns — and is what
/// every headline number in SPEC §11 is measured at.
[[nodiscard]] std::int32_t strideFrom(int argc, char** argv, std::int32_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--stride") == 0) {
            const int value = std::atoi(argv[i + 1]);
            if (value > 0 && value <= 32) {
                return value;
            }
        }
    }
    return fallback;
}

[[nodiscard]] bool hasFlag(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i]) {
            return true;
        }
    }
    return false;
}

void printCensus(const char* what, const RuleGraph& graph, const Decoder& decoder,
                 const std::vector<ConditionTally>& tallies) {
    std::printf("  %s: which (noise, threshold) the placed block decides\n", what);
    std::printf("  %-42s %11s %11s %9s %7s %7s\n", "condition", "positions", "masked", "columns",
                "true%", "contra");
    for (std::size_t i = 0; i < tallies.size(); ++i) {
        const auto index = static_cast<stratum::surface::ConditionIndex>(i);
        if (decoder.noiseOf(index) < 0) {
            continue;
        }
        const ConditionTally& tally = tallies[i];
        std::printf("  %-42s %11zu %11zu %9zu %6.2f%% %7zu\n",
                    describeCondition(graph, index).c_str(), tally.observed, tally.touchedOnly,
                    tally.columns, percent(tally.trueBits, tally.observed), tally.contradictions);
    }
}

[[nodiscard]] int runControl(const std::filesystem::path& root, const Pack& pack,
                             const stratum::settings::LoadedSettings& loaded,
                             const NoiseSettings& settings, const ResourceLocation& id,
                             std::int32_t stride, bool enumerateDepth, bool census, bool trustSteep,
                             bool trustWater, bool replayGate) {
    const RuleGraph graph = RuleGraph::resolve(settings.surfaceRule, id);
    const Decoder decoder{graph, settings};
    std::vector<ResourceLocation> wanted = stratum::surface::requiredNoises(graph);
    for (const ResourceLocation& name : loaded.graph.referencedNoises()) {
        wanted.push_back(name);
    }
    std::ranges::sort(wanted);
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    std::map<std::string, ControlArm> recovery;
    std::map<std::string, ControlArm> negative;
    WalkStats total;
    std::size_t worlds = 0;
    std::vector<ConditionTally> pooled(graph.conditionCount());
    const stratum::biome::TemperatureTable temperatures =
        stratum::biome::TemperatureTable::fromPack(pack);

    for (const std::int64_t seed : kGoldenSeeds) {
        const std::filesystem::path region = legacy_goldens::regionOf(root, seed, "overworld");
        if (region.empty()) {
            std::printf("  seed %-21lld  MISSING overworld region\n", static_cast<long long>(seed));
            continue;
        }
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, wanted, seed, stratum::density::RandomSource::Xoroshiro);
        const auto wrongNoises = stratum::density::NoiseRegistry::create(
            pack, wanted, seed + 1, stratum::density::RandomSource::Xoroshiro);
        const stratum::surface::Executor executor = stratum::surface::Executor::compile(
            graph, seed, settings.geometry, &noises, settings.seaLevel);
        const stratum::noise::NormalNoise& secondary =
            noises.get(ResourceLocation::parse("minecraft:surface_secondary"));
        PreliminarySurface preliminary{loaded.graph, noises, settings};
        const Replay replay{
            .executor = &executor, .temperatures = &temperatures, .gate = replayGate};

        Walk walk;
        walk.tallies.assign(graph.conditionCount(), {});
        walk.fields.assign(graph.conditionCount(), {});
        walk.keepFields = true;
        walkRegion(
            region, settings, decoder, stride, trustSteep, replay,
            [&](std::int32_t x, std::int32_t z) {
                Known known;
                if (!enumerateDepth) {
                    known.surfaceDepth = executor.surfaceDepth(x, z);
                }
                known.surfaceSecondary =
                    secondary.sample(static_cast<double>(x), 0.0, static_cast<double>(z));
                known.preliminarySurface = preliminary.at(x, z);
                return known;
            },
            walk);

        scoreAgainstRegistry(graph, decoder, noises, walk, recovery);
        scoreAgainstRegistry(graph, decoder, wrongNoises, walk, negative);
        total.positions += walk.stats.positions;
        total.unexplained += walk.stats.unexplained;
        total.overflowed += walk.stats.overflowed;
        total.noBiome += walk.stats.noBiome;
        total.replayed += walk.stats.replayed;
        total.replayAgreed += walk.stats.replayAgreed;
        for (std::size_t i = 0; i < pooled.size(); ++i) {
            pooled[i].observed += walk.tallies[i].observed;
            pooled[i].touchedOnly += walk.tallies[i].touchedOnly;
            pooled[i].trueBits += walk.tallies[i].trueBits;
            pooled[i].columns += walk.tallies[i].columns;
            pooled[i].contradictions += walk.tallies[i].contradictions;
        }
        ++worlds;
    }

    if (worlds == 0) {
        std::printf("no overworld golden regions under %s\n", root.string().c_str());
        return 77;
    }

    std::printf("\ncontrol: the MODERN seeding this build already reproduces, through the "
                "identical decoder\n");
    std::printf("  water           %s\n",
                trustWater ? "an interval: the golden's topmost fluid, up through any "
                             "rule-placeable solid above it"
                           : "BRANCHED ON");
    std::printf("  steep           %s\n", trustSteep
                                              ? "taken from the golden's own heights"
                                              : "BRANCHED ON — the golden shows post-rule heights");
    std::printf("  surface depth   %s\n",
                enumerateDepth ? "ENUMERATED [-4, 11], exactly as the Nether run does"
                               : "known exactly (minecraft:surface is modern here)");
    std::printf("  regions %zu   positions decoded %zu\n", worlds, total.positions);
    std::printf("  unexplained by the tree %zu (%.4f%%)   assignment cap hit %zu   no biome %zu\n",
                total.unexplained, percent(total.unexplained, total.positions), total.overflowed,
                total.noBiome);
    std::printf("  replay: the library's own Executor on the reconstructed Context reproduces "
                "%zu / %zu golden blocks (%.4f%%)%s\n",
                total.replayAgreed, total.replayed, percent(total.replayAgreed, total.replayed),
                replayGate ? "; only those positions are scored" : "; all positions are scored");

    if (census) {
        printCensus("overworld", graph, decoder, pooled);
    }

    std::size_t bits = 0;
    std::size_t agreed = 0;
    std::size_t majority = 0;
    std::size_t wrongAgreed = 0;
    std::printf("  %-34s %10s %10s %8s %8s\n", "noise", "bits", "recovery", "majority", "seed+1");
    for (const auto& [name, arm] : recovery) {
        const ControlArm& wrong = negative[name];
        std::printf("  %-34s %10zu %9.4f%% %7.2f%% %7.2f%%\n", name.c_str(), arm.bits,
                    percent(arm.agreed, arm.bits), percent(arm.majority, arm.bits),
                    percent(wrong.agreed, wrong.bits));
        bits += arm.bits;
        agreed += arm.agreed;
        majority += arm.majority;
        wrongAgreed += wrong.agreed;
    }
    std::printf("  %-34s %10zu %9.4f%% %7.2f%% %7.2f%%\n", "ALL", bits, percent(agreed, bits),
                percent(majority, bits), percent(wrongAgreed, bits));
    for (const auto& [name, arm] : recovery) {
        for (const std::string& sample : arm.samples) {
            std::printf("    disagreement: %s\n", sample.c_str());
        }
    }
    // The wrong-seed arm has to sit AT the trivial predictor, not merely
    // below some round number: these bits are lopsided, so "78.6%" is only
    // meaningful beside the 78.7% a constant answer already scores.
    const bool pass = bits > 0 && percent(agreed, bits) > 99.9 &&
                      percent(wrongAgreed, bits) <= percent(majority, bits) + 1.0;
    std::printf("  control %s\n", pass ? "PASSES" : "FAILS");
    return pass ? 0 : 1;
}

// -------------------------------------------------------------- the nether

/// Decodes the Nether goldens once and hands back the per-column fields.
struct NetherRun {
    RuleGraph graph;
    std::vector<Field> fields; ///< one per condition, unioned over all worlds
    std::vector<ConditionTally> tallies;
    WalkStats stats;
    std::size_t worlds = 0;
    /// Per world, per condition: kept apart so a claim can be made per world
    /// rather than only over the pool.
    std::vector<std::pair<std::int64_t, std::vector<Field>>> perWorld;
};

[[nodiscard]] std::optional<NetherRun> runNether(const std::filesystem::path& root,
                                                 const NoiseSettings& settings,
                                                 const ResourceLocation& id, std::int32_t stride) {
    NetherRun run{.graph = RuleGraph::resolve(settings.surfaceRule, id),
                  .fields = {},
                  .tallies = {},
                  .stats = {},
                  .worlds = 0,
                  .perWorld = {}};
    const Decoder decoder{run.graph, settings};
    run.tallies.assign(run.graph.conditionCount(), {});

    for (const std::int64_t seed : kGoldenSeeds) {
        const std::filesystem::path region = legacy_goldens::regionOf(root, seed, "nether");
        if (region.empty()) {
            std::printf("  seed %-21lld  MISSING nether region\n", static_cast<long long>(seed));
            continue;
        }
        Walk walk;
        walk.tallies.assign(run.graph.conditionCount(), {});
        walk.fields.assign(run.graph.conditionCount(), {});
        walk.keepFields = true;
        walkRegion(
            region, settings, decoder, stride, false, Replay{},
            [](std::int32_t, std::int32_t) { return Known{}; }, walk);
        for (std::size_t i = 0; i < run.tallies.size(); ++i) {
            run.tallies[i].observed += walk.tallies[i].observed;
            run.tallies[i].touchedOnly += walk.tallies[i].touchedOnly;
            run.tallies[i].trueBits += walk.tallies[i].trueBits;
            run.tallies[i].columns += walk.tallies[i].columns;
            run.tallies[i].contradictions += walk.tallies[i].contradictions;
        }
        run.stats.positions += walk.stats.positions;
        run.stats.unexplained += walk.stats.unexplained;
        run.stats.overflowed += walk.stats.overflowed;
        run.stats.noBiome += walk.stats.noBiome;
        run.perWorld.emplace_back(seed, std::move(walk.fields));
        ++run.worlds;
    }
    if (run.worlds == 0) {
        return std::nullopt;
    }
    return run;
}

void printDecodeCensus(const NetherRun& run, const Decoder& decoder) {
    std::printf("\nnether readback: which (noise, threshold) the placed block decides\n");
    std::printf("  regions %zu (SIX independent worlds at stride 1)   positions decoded %zu\n",
                run.worlds, run.stats.positions);
    std::printf("  unexplained by the tree %zu (%.4f%%)   assignment cap hit %zu\n",
                run.stats.unexplained, percent(run.stats.unexplained, run.stats.positions),
                run.stats.overflowed);
    printCensus("nether", run.graph, decoder, run.tallies);
}

/// The identity test. `soul_sand_layer` and `gravel_layer` have byte-identical
/// parameters — firstOctave -8, amplitudes [1,1,1,1,0,0,0,0,0.0133...] — and
/// differ only by name, and both are read at exactly one threshold (-0.012).
/// If the name does not enter the seed they are ONE field and their decoded
/// signs must agree on every column that decides both.
void printIdentity(const NetherRun& run, const Decoder& decoder) {
    stratum::surface::ConditionIndex soul = 0;
    stratum::surface::ConditionIndex gravel = 0;
    bool haveSoul = false;
    bool haveGravel = false;
    for (stratum::surface::ConditionIndex i = 0; i < run.graph.conditionCount(); ++i) {
        if (decoder.noiseOf(i) < 0) {
            continue;
        }
        const std::string name = run.graph.condition(i).noise->toString();
        if (name == "minecraft:soul_sand_layer") {
            soul = i;
            haveSoul = true;
        } else if (name == "minecraft:gravel_layer") {
            gravel = i;
            haveGravel = true;
        }
    }
    std::printf("\nidentity test: minecraft:soul_sand_layer against minecraft:gravel_layer\n");
    if (!haveSoul || !haveGravel) {
        std::printf("  one of the two is not read by this tree — nothing to compare\n");
        return;
    }
    std::array<std::size_t, 4> joint{};
    std::size_t soulTrue = 0;
    std::size_t soulBits = 0;
    std::size_t gravelTrue = 0;
    std::size_t gravelBits = 0;
    for (const auto& [seed, fields] : run.perWorld) {
        std::size_t worldBoth = 0;
        std::size_t worldSame = 0;
        for (std::size_t slot = 0; slot < legacy_goldens::kColumnsPerRegion; ++slot) {
            const std::int8_t a = fields[soul].bits[slot];
            const std::int8_t b = fields[gravel].bits[slot];
            if (a >= 0) {
                ++soulBits;
                soulTrue += static_cast<std::size_t>(a == 1 ? 1 : 0);
            }
            if (b >= 0) {
                ++gravelBits;
                gravelTrue += static_cast<std::size_t>(b == 1 ? 1 : 0);
            }
            if (a < 0 || b < 0) {
                continue;
            }
            ++joint[(static_cast<std::size_t>(a) * 2U) + static_cast<std::size_t>(b)];
            ++worldBoth;
            if (a == b) {
                ++worldSame;
            }
        }
        std::printf("  seed %-21lld  %zu / %zu agree  (%.4f%%)\n", static_cast<long long>(seed),
                    worldSame, worldBoth, percent(worldSame, worldBoth));
    }
    const std::size_t both = joint[0] + joint[1] + joint[2] + joint[3];
    const std::size_t same = joint[0] + joint[3];
    std::printf("  pooled          %zu / %zu agree  (%.4f%%)\n", same, both, percent(same, both));
    std::printf("  joint table     (soul, gravel) = (F,F) %zu  (F,T) %zu  (T,F) %zu  (T,T) %zu\n",
                joint[0], joint[1], joint[2], joint[3]);
    std::printf("  marginals       soul_sand_layer >= -0.012 in %.2f%% of %zu columns, "
                "gravel_layer in %.2f%% of %zu\n",
                percent(soulTrue, soulBits), soulBits, percent(gravelTrue, gravelBits), gravelBits);
    std::printf("  ONE FIELD makes (F,T) and (T,F) IMPOSSIBLE — they are %zu of %zu (%.4f%%).\n",
                joint[1] + joint[2], both, percent(joint[1] + joint[2], both));
}

// ----------------------------------------------------------------- the scan

struct Candidate {
    std::size_t rule = 0;
    std::size_t block = 0;
    std::size_t agreed = 0;
};

/// The columns one condition decides, pooled over every world, as
/// (worldSeed, x, z, bit). The scan scores a candidate against these.
struct Targets {
    std::vector<std::int64_t> seed;
    std::vector<std::int32_t> x;
    std::vector<std::int32_t> z;
    std::vector<std::int8_t> bit;
    double threshold = 0.0;
    double maxThreshold = 0.0;
    std::string noise;
    /// Columns two DIFFERENT conditions on this noise decoded both ways.
    /// Another measured error bound, and an independent one: these positions
    /// are not just in the same column, they are read through different
    /// branches of the tree.
    std::size_t crossContradictions = 0;
};

/// Pools one noise's decoded bits over every condition that reads it and
/// every world, ONE ENTRY PER COLUMN rather than one per condition.
///
/// The Nether's tree reads each of these noises at a single threshold from
/// several places — `nether_state_selector >= 0` appears three times — so a
/// column decided by two of them is the SAME bit, and counting it twice would
/// inflate the denominator with a copy of itself. Conditions on one noise at
/// DIFFERENT intervals are refused rather than merged: they are different
/// questions about the same value, and no dimension in play has any.
[[nodiscard]] Targets targetsFor(const NetherRun& run, const Decoder& decoder,
                                 std::string_view noiseName) {
    Targets targets;
    std::vector<stratum::surface::ConditionIndex> conditions;
    for (stratum::surface::ConditionIndex i = 0; i < run.graph.conditionCount(); ++i) {
        if (decoder.noiseOf(i) < 0) {
            continue;
        }
        const stratum::surface::Condition& condition = run.graph.condition(i);
        if (!condition.noise.has_value() || condition.noise->toString() != noiseName) {
            continue;
        }
        if (!conditions.empty() &&
            (std::abs(condition.minThreshold - targets.threshold) > 0.0 ||
             std::abs(condition.maxThreshold - targets.maxThreshold) > 0.0)) {
            throw std::runtime_error("'" + std::string{noiseName} +
                                     "' is read at two different thresholds; pooling them would "
                                     "merge two different questions");
        }
        targets.noise = noiseName;
        targets.threshold = condition.minThreshold;
        targets.maxThreshold = condition.maxThreshold;
        conditions.push_back(i);
    }
    for (const auto& [seed, fields] : run.perWorld) {
        std::vector<std::int8_t> merged(legacy_goldens::kColumnsPerRegion, -1);
        for (const stratum::surface::ConditionIndex i : conditions) {
            for (std::size_t slot = 0; slot < merged.size(); ++slot) {
                const std::int8_t bit = fields[i].bits[slot];
                if (bit < 0) {
                    continue;
                }
                if (merged[slot] >= 0 && merged[slot] != bit) {
                    merged[slot] = Field::kPoisoned;
                    ++targets.crossContradictions;
                } else if (merged[slot] != Field::kPoisoned) {
                    merged[slot] = bit;
                }
            }
        }
        for (std::size_t slot = 0; slot < merged.size(); ++slot) {
            if (merged[slot] < 0) {
                continue;
            }
            targets.seed.push_back(seed);
            targets.x.push_back(static_cast<std::int32_t>(slot % 512U));
            targets.z.push_back(static_cast<std::int32_t>(slot / 512U));
            targets.bit.push_back(merged[slot]);
        }
    }
    return targets;
}

/// Reads the noise's declared parameters straight out of the pack, so the
/// octave layout is the pinned version's rather than this file's memory of it.
[[nodiscard]] stratum::density::NoiseParameters parametersOf(const Pack& pack,
                                                             const std::string& noiseName) {
    const auto id = ResourceLocation::parse(noiseName);
    const stratum::data::PackEntry* entry = pack.find(stratum::data::Registry::Noise, id);
    if (entry == nullptr) {
        throw std::runtime_error("the pack defines no worldgen/noise '" + noiseName + "'");
    }
    return stratum::density::NoiseParameters::fromJson(entry->json, id);
}

/// A thinned sample of the targets, so 270,000 candidates can be scored at
/// all. A correct rule agrees on essentially every column, so it survives any
/// thinning; the survivors are then rescored on the whole set.
[[nodiscard]] std::vector<std::size_t> thin(const Targets& targets, std::size_t wanted) {
    std::vector<std::size_t> picked;
    if (targets.bit.empty()) {
        return picked;
    }
    const std::size_t step = std::max<std::size_t>(1, targets.bit.size() / wanted);
    for (std::size_t i = 0; i < targets.bit.size(); i += step) {
        picked.push_back(i);
    }
    return picked;
}

[[nodiscard]] std::size_t
scoreCandidate(const Targets& targets, const std::vector<std::size_t>& picked,
               const legacy_goldens::Layout& layout,
               const std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>>& blocks,
               std::size_t offset) {
    std::size_t agreed = 0;
    for (const std::size_t i : picked) {
        const auto found = blocks.find(targets.seed[i]);
        const double value = legacy_goldens::sampleNormal(layout, found->second, offset,
                                                          static_cast<double>(targets.x[i]), 0.0,
                                                          static_cast<double>(targets.z[i]));
        const bool holds = targets.threshold <= value && value <= targets.maxThreshold;
        if (holds == (targets.bit[i] == 1)) {
            ++agreed;
        }
    }
    return agreed;
}

void runScan(const Pack& pack, const NetherRun& run, const Decoder& decoder,
             const std::string& noiseName, std::size_t sampleSize, std::size_t plantRule,
             std::size_t plantBlock, bool plant) {
    Targets targets = targetsFor(run, decoder, noiseName);
    if (targets.bit.empty()) {
        std::printf("\nscan %s: the decoder observes it at NO column — nothing to score\n",
                    noiseName.c_str());
        return;
    }
    const std::size_t constantTrue =
        static_cast<std::size_t>(std::ranges::count(targets.bit, static_cast<std::int8_t>(1)));
    if (constantTrue == 0 || constantTrue == targets.bit.size()) {
        std::printf("\nscan %s: every one of the %zu decoded columns reads the SAME way "
                    "(%s). A constant bit carries no information about any seeding: its null "
                    "and its signal are the same number, so it is not scanned.\n",
                    noiseName.c_str(), targets.bit.size(),
                    constantTrue == 0 ? "below the threshold" : "at or above the threshold");
        return;
    }
    const stratum::density::NoiseParameters parameters = parametersOf(pack, noiseName);
    const legacy_goldens::Layout layout =
        legacy_goldens::layoutFor(parameters.firstOctave, parameters.amplitudes);
    const std::vector<std::size_t> picked = thin(targets, sampleSize);

    std::size_t trueBits = 0;
    for (const std::size_t i : picked) {
        trueBits += static_cast<std::size_t>(targets.bit[i] == 1 ? 1 : 0);
    }

    std::printf("\nscan %s  (firstOctave %d, %zu amplitudes, %zu Perlin blocks per noise)\n",
                noiseName.c_str(), parameters.firstOctave, parameters.amplitudes.size(),
                layout.blocksPerNoise);
    std::printf("  decoded columns %zu over %zu worlds (%zu dropped: two branches of the tree "
                "read the same column both ways); scored on %zu of them\n",
                targets.bit.size(), run.worlds, targets.crossContradictions, picked.size());
    std::printf("  candidates %zu seed rules x %zu block offsets = %zu\n",
                legacy_goldens::kSeedRules, legacy_goldens::kBlockOffsets,
                legacy_goldens::kSeedRules * legacy_goldens::kBlockOffsets);
    std::printf("  the MAJORITY-CLASS null is %.2f%% (this bit is %.2f%% true)\n",
                percent(std::max(trueBits, picked.size() - trueBits), picked.size()),
                percent(trueBits, picked.size()));

    std::vector<std::int64_t> seeds;
    seeds.reserve(run.perWorld.size());
    for (const auto& [seed, fields] : run.perWorld) {
        seeds.push_back(seed);
    }

    // THE CALIBRATION ARM. `--plant` replaces the server's decoded bits with
    // the bits a chosen candidate WOULD have produced, and then runs the
    // identical scan. If the planted candidate does not come back first and
    // at 100%, this scan cannot find a correct rule at this candidate count
    // and no refutation drawn from it means anything. Unlike
    // legacy-seed-analyze.cpp's plant there is no quantisation to go through:
    // a decoded bit IS the sign of the noise against the threshold, so a
    // correct rule scores exactly every column or the scorer is broken.
    if (plant) {
        const legacy_goldens::SeedRule rule = legacy_goldens::ruleAt(plantRule);
        std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> planted;
        for (const std::int64_t seed : seeds) {
            planted.emplace(seed, legacy_goldens::blocksFor(
                                      rule, legacy_goldens::seedFor(rule, seed, noiseName),
                                      plantBlock + layout.blocksPerNoise));
        }
        for (std::size_t i = 0; i < targets.bit.size(); ++i) {
            const double value = legacy_goldens::sampleNormal(
                layout, planted.at(targets.seed[i]), plantBlock, static_cast<double>(targets.x[i]),
                0.0, static_cast<double>(targets.z[i]));
            targets.bit[i] = static_cast<std::int8_t>(
                targets.threshold <= value && value <= targets.maxThreshold ? 1 : 0);
        }
        std::printf("  PLANTED rule %zu block %zu in place of the server's bits\n", plantRule,
                    plantBlock);
    }

    std::vector<Candidate> best;
    std::size_t sum = 0;
    std::size_t total = 0;
    std::size_t peak = 0;
    for (std::size_t ruleIndex = 0; ruleIndex < legacy_goldens::kSeedRules; ++ruleIndex) {
        const legacy_goldens::SeedRule rule = legacy_goldens::ruleAt(ruleIndex);
        std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> blocks;
        for (const std::int64_t seed : seeds) {
            blocks.emplace(seed, legacy_goldens::blocksFor(
                                     rule, legacy_goldens::seedFor(rule, seed, noiseName),
                                     legacy_goldens::kBlockOffsets + layout.blocksPerNoise));
        }
        for (std::size_t offset = 0; offset < legacy_goldens::kBlockOffsets; ++offset) {
            const std::size_t agreed = scoreCandidate(targets, picked, layout, blocks, offset);
            sum += agreed;
            ++total;
            peak = std::max(peak, agreed);
            if (best.size() < 8 || agreed > best.back().agreed) {
                best.push_back({.rule = ruleIndex, .block = offset, .agreed = agreed});
                std::ranges::sort(best, [](const Candidate& a, const Candidate& b) {
                    return a.agreed > b.agreed;
                });
                if (best.size() > 8) {
                    best.pop_back();
                }
            }
        }
    }

    std::printf("  mean agreement over the whole space %.4f%%   maximum %.4f%%\n",
                percent(sum / std::max<std::size_t>(total, 1), picked.size()),
                percent(peak, picked.size()));
    std::printf("  best eight, on the thinned sample and then on EVERY decoded column:\n");

    // THE SECOND STAGE, and it is not a formality. A candidate noise is
    // SPATIALLY SMOOTH and the decoded columns are spatially CLUSTERED, so
    // the agreement of a wrong candidate is nothing like a fair coin over
    // independent trials: the best of 270000 wrong rules reaches far past
    // what sqrt(n) would suggest on a thinned sample. Rescoring the leaders
    // on the whole set is what separates "agrees on a patch" from "agrees" —
    // a correct rule does not regress, and every one of these does.
    std::vector<std::size_t> all(targets.bit.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    for (std::size_t i = 0; i < best.size(); ++i) {
        const legacy_goldens::SeedRule rule = legacy_goldens::ruleAt(best[i].rule);
        std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> leader;
        for (const std::int64_t seed : seeds) {
            leader.emplace(seed, legacy_goldens::blocksFor(
                                     rule, legacy_goldens::seedFor(rule, seed, noiseName),
                                     best[i].block + layout.blocksPerNoise));
        }
        const std::size_t full = scoreCandidate(targets, all, layout, leader, best[i].block);
        std::printf("    #%zu rule %zu block %zu: %zu/%zu %.4f%% thinned, %zu/%zu %.4f%% full  "
                    "%s\n",
                    i + 1, best[i].rule, best[i].block, best[i].agreed, picked.size(),
                    percent(best[i].agreed, picked.size()), full, targets.bit.size(),
                    percent(full, targets.bit.size()), legacy_goldens::describe(rule).c_str());
    }

    // deepslate's own derivation, by name rather than left to be inferred
    // from a list it is absent from.
    const legacy_goldens::SeedRule named = legacy_goldens::ruleAt(legacy_goldens::kDeepslateRule);
    std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> blocks;
    for (const std::int64_t seed : seeds) {
        blocks.emplace(
            seed, legacy_goldens::blocksFor(named, legacy_goldens::seedFor(named, seed, noiseName),
                                            layout.blocksPerNoise));
    }
    const std::size_t namedAgreed = scoreCandidate(targets, all, layout, blocks, 0);
    std::printf("  rule %zu block 0 (deepslate's own derivation): %zu/%zu  %.4f%%  %s\n",
                legacy_goldens::kDeepslateRule, namedAgreed, targets.bit.size(),
                percent(namedAgreed, targets.bit.size()), legacy_goldens::describe(named).c_str());

    // The full-set majority-class null, which is what the "full" column above
    // has to be read against.
    std::size_t allTrue = 0;
    for (const std::int8_t bit : targets.bit) {
        allTrue += static_cast<std::size_t>(bit == 1 ? 1 : 0);
    }
    std::printf("  on all %zu columns the majority-class null is %.4f%%\n", targets.bit.size(),
                percent(std::max(allTrue, targets.bit.size() - allTrue), targets.bit.size()));

    // THE NULL, MEASURED ON THE SAME COLUMNS THE LEADERS ARE. A decoded
    // column set is spatially CLUSTERED and a candidate noise is spatially
    // SMOOTH, so a wrong candidate's agreement is nothing like a binomial
    // draw: its effective sample size is the number of independent patches,
    // not the number of columns. Quoting sqrt(n) here would turn every
    // leader into an impossible outlier. This scores an evenly spaced
    // thousandth of the space on the FULL column set and reports what wrong
    // rules actually reach, which is the only number the leaders mean
    // anything against.
    constexpr std::size_t kNullStride = 271;
    std::size_t nullCount = 0;
    double nullSum = 0.0;
    double nullSquares = 0.0;
    std::size_t nullMax = 0;
    for (std::size_t index = 0; index < legacy_goldens::kSeedRules * legacy_goldens::kBlockOffsets;
         index += kNullStride) {
        const std::size_t ruleIndex = index / legacy_goldens::kBlockOffsets;
        const std::size_t offset = index % legacy_goldens::kBlockOffsets;
        const legacy_goldens::SeedRule rule = legacy_goldens::ruleAt(ruleIndex);
        std::map<std::int64_t, std::vector<stratum::noise::PerlinNoise>> drawn;
        for (const std::int64_t seed : seeds) {
            drawn.emplace(seed, legacy_goldens::blocksFor(
                                    rule, legacy_goldens::seedFor(rule, seed, noiseName),
                                    offset + layout.blocksPerNoise));
        }
        const std::size_t agreed = scoreCandidate(targets, all, layout, drawn, offset);
        const double fraction = static_cast<double>(agreed) / static_cast<double>(all.size());
        nullSum += fraction;
        nullSquares += fraction * fraction;
        nullMax = std::max(nullMax, agreed);
        ++nullCount;
    }
    const double nullMean = nullSum / static_cast<double>(nullCount);
    const double nullSd = std::sqrt(
        std::max(0.0, (nullSquares / static_cast<double>(nullCount)) - (nullMean * nullMean)));
    std::printf("  MEASURED NULL on the same columns, %zu candidates every %zuth of the space: "
                "mean %.4f%%  sd %.4f points  max %.4f%%\n",
                nullCount, kNullStride, nullMean * 100.0, nullSd * 100.0,
                percent(nullMax, all.size()));
}

/// The whole of main, so that an exception carrying a fixture path or a
/// registry name reaches the terminal as a message rather than as a
/// `terminate` with nothing said: SPEC 8's rule about failing loudly is
/// about the message, not only the exit code.
[[nodiscard]] int run(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <fixtures-root> "
                    "--control|--decode|--identity|--scan <noise>|--scan-all\n"
                    "       [--stride N] [--plant <rule> <block>] [--census] [--trust-steep]\n"
                    "       [--symbolic-water] [--enumerate] [--no-replay-gate]\n",
                    argv[0]);
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    const std::filesystem::path tree = legacy_goldens::findWorldgenTree(root);
    if (tree.empty()) {
        std::printf("no extracted vanilla worldgen under %s — Mojang-derived and never "
                    "committed (SPEC 12). Generate it with tools/fetch-vanilla.\n",
                    root.string().c_str());
        return 77;
    }

    const Pack pack = Pack::open(tree);
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);

    if (hasFlag(argc, argv, "--control")) {
        const auto id = ResourceLocation::parse("minecraft:overworld");
        return runControl(root, pack, loaded, loaded.settings.at(id), id, strideFrom(argc, argv, 1),
                          hasFlag(argc, argv, "--enumerate"), hasFlag(argc, argv, "--census"),
                          hasFlag(argc, argv, "--trust-steep"),
                          !hasFlag(argc, argv, "--symbolic-water"),
                          !hasFlag(argc, argv, "--no-replay-gate"));
    }

    const auto netherId = ResourceLocation::parse("minecraft:nether");
    const NoiseSettings& nether = loaded.settings.at(netherId);
    const std::optional<NetherRun> run =
        runNether(root, nether, netherId, strideFrom(argc, argv, 1));
    if (!run.has_value()) {
        std::printf("no nether golden regions under %s\n", root.string().c_str());
        return 77;
    }
    const Decoder decoder{run->graph, nether};

    if (hasFlag(argc, argv, "--decode")) {
        printDecodeCensus(*run, decoder);
        printIdentity(*run, decoder);
        return 0;
    }
    if (hasFlag(argc, argv, "--identity")) {
        printIdentity(*run, decoder);
        return 0;
    }
    std::size_t plantRule = 0;
    std::size_t plantBlock = 0;
    bool plant = false;
    for (int i = 1; i + 2 < argc; ++i) {
        if (std::strcmp(argv[i], "--plant") == 0) {
            plantRule = static_cast<std::size_t>(std::atoi(argv[i + 1]));
            plantBlock = static_cast<std::size_t>(std::atoi(argv[i + 2]));
            plant = true;
        }
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--scan") == 0) {
            printDecodeCensus(*run, decoder);
            printIdentity(*run, decoder);
            runScan(pack, *run, decoder, argv[i + 1], 512, plantRule, plantBlock, plant);
            return 0;
        }
    }
    if (hasFlag(argc, argv, "--scan-all")) {
        printDecodeCensus(*run, decoder);
        printIdentity(*run, decoder);
        // One decode, every noise: the walk is the expensive half and there is
        // no reason to repeat it six times.
        std::vector<std::string> names;
        for (stratum::surface::ConditionIndex i = 0; i < run->graph.conditionCount(); ++i) {
            if (decoder.noiseOf(i) < 0) {
                continue;
            }
            const std::string name = run->graph.condition(i).noise->toString();
            if (std::ranges::find(names, name) == names.end()) {
                names.push_back(name);
            }
        }
        for (const std::string& name : names) {
            runScan(pack, *run, decoder, name, 512, plantRule, plantBlock, plant);
        }
        return 0;
    }
    std::printf("no mode given\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::printf("failed: %s\n", error.what());
        return 1;
    }
}
