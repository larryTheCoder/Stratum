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
// EIGHT REGION FILES, SIX WORLDS — AND EVERY DENOMINATOR HERE POOLS TWO
// COPIES. java.util.Random scrambles a seed as
// `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, which keeps only its low 48 bits
// — the TOP SIXTEEN, not bit 63 alone — so 0 == Long.MIN_VALUE and
// -1 == Long.MAX_VALUE to every legacy-seeded noise. Two of the eight region
// files are therefore a second copy of a world already in the pool: a QUARTER
// of the files, and a comparable share of every pooled column count below —
// measured per noise at stride 4, netherrack 21.73%, nether_state_selector
// 25.58%, patch 31.27%, soul_sand_layer 38.47%, gravel_layer 40.59%.
// Measured rather than assumed — over one Nether region's 67108864 block
// positions the two members of a pair agree on the air/fluid/solid CATEGORY of
// every single one and on every biome, and differ in 11914 block NAMES
// (0.0178%, in 142 of 1024 chunks), which is a feature-sized residue: features
// are seeded from the whole 64-bit seed and run after the surface rules.
//
// THAT IDENTITY IS ALSO EVIDENCE, before any scan. A candidate base that
// RETAINS the top sixteen bits — `worldSeed` raw, and `xoroLo`, which is
// Xoroshiro128++ seeded from the full 64-bit value — predicts a DIFFERENT
// surface-noise field for seed 0 than for Long.MIN_VALUE (measured: worldSeed
// gives 0000000000000000 against 8000000000000000, xoroLo 2a2ca488f66f517e
// against 4d272cf8f66aec27). The goldens show the same field. So 2 of the
// space's 5 bases — 360 of the 900 seed rules, 108000 of the 270000
// candidates — are refuted by the fixtures themselves, and the scan below is
// not what does it. `lcgLong`, `scrambled` and `zero` agree across each pair,
// as they must.
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
// AND THEN THE SAME THREE ARMS WITH THE SURFACE DEPTH ENUMERATED, which is the
// arm that matters and which this case did not have until now. The three
// numbers above come from a walk that is HANDED the column's exact surface
// depth. The Nether run cannot be: `minecraft:surface` is itself a
// legacy-seeded named noise there, so the depth is enumerated over [-4, 11]
// and a bit survives only where all sixteen assignments agree. That is a
// different, strictly weaker path through the same walker, and a control that
// only ever took the supplied-depth path was validating code the measurement
// does not run. The enumerated arm is run here over the same regions, held to
// the same bar — 100.0000% recovery, a wrong seeding at the trivial predictor,
// 0 positions unexplained — and, being weaker, on strictly fewer bits, which
// the case asserts rather than hopes for.
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
// AND THE SCAN CAN FIND A CORRECT RULE, BY RANK AND NOT BY RECOVERY.
// `--plant` replaces the server's bits with the bits a chosen candidate would
// have produced and runs the identical scan. Checking only that the planted
// rule then scores 100% would be near-tautological — it is scoring bits it
// just wrote. What this case asserts is RANK: the planted candidate comes back
// FIRST in the same 1800-candidate sweep that found nothing on the server's
// bits, at 100.0000% of every decoded column, with the best wrong rule in that
// sweep far below it. So "no survivor" is a measurement of the space, not a
// property of the apparatus.
//
// THE IDENTITY TEST is the one thing here that is not a null result.
// `minecraft:soul_sand_layer` and `minecraft:gravel_layer` have byte-identical
// parameters — firstOctave -8, amplitudes [1,1,1,1,0,0,0,0,0.01333...] — and
// differ only by name, and the Nether's `nether_wastes` branch reads both at
// -0.012 in a way that decides BOTH at the same column.
//
// WHICH CELL CARRIES THE CLAIM, derived from the resolved tree rather than
// from the shape of the table. `nether_wastes` is a two-child SEQUENCE:
//
//   child 0  gate `stone_depth(floor, add_surface_depth: true)` — depth <=
//            surfaceDepth — then `soul_sand_layer >= -0.012`, whose `then_run`
//            ends in an unconditional `netherrack`. Entered with the noise at
//            or above the threshold, it ALWAYS places.
//   child 1  gate `stone_depth(floor, add_surface_depth: false)` — depth <= 0
//            — then y bounds, then `gravel_layer >= -0.012`.
//
// The gates are DIFFERENT and not nested, so "gravel is only consulted after
// soul failed" is not the reason the (T,*) row is empty — child 1 is reached
// whenever child 0 placed nothing, INCLUDING where child 0's depth gate failed
// and `soul_sand_layer` was never consulted. The actual reason is that child
// 1's gate needs depth 0, which is the MOST PERMISSIVE depth for child 0's
// gate: where surfaceDepth >= 0 child 0 was entered first at that position and
// only a FALSE soul bit let control through, and where surfaceDepth < 0 child
// 0's gate fails at every depth in the column, so soul is never consulted
// there at all. Either way a column cannot decide soul TRUE and gravel
// anything, for ANY fixed surface depth.
//
// So the seeding claim rests on (F,T) — soul below, gravel at or above —
// which is impossible under ONE FIELD because both conditions test the same
// value at the same (x, 0, z) against the same -0.012 whichever branch reached
// them, and is the EXPECTED cell under the tree. The joint table at stride 1
// is (F,F) 1800, (F,T) 1398, (T,F) 0, (T,T) 6: the deciding cell is 1398 of
// 3204 columns, 43.63%. Something about the identifier — its hash, or the
// order the noises are built in, which this cannot separate — reaches the
// seed.
//
// (T,F) and (T,T) are impossible under the TREE as well, so the 6 columns
// there are the decoder or the reconstruction being wrong — a FLOOR on this
// readback's error rate and not a measurement of it, since an error landing in
// (F,F) or (F,T) leaves no trace in this table at all. It is not an upper
// bound either. The mechanism is known: the decoder enumerates the surface
// depth PER POSITION and unions the resulting bits per column, so nothing
// forces one depth across a column, and two positions can be decoded under
// depths that could not both have been true.
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
/// region files, so the default takes every eighth chunk in both axes: 16 of
/// each region's 1024. STRATUM_GOLDEN_SURFACE_STRIDE=1 runs the whole thing,
/// which is what the numbers quoted in SPEC §11 are measured at.
constexpr std::int32_t kDefaultStride = 8;

[[nodiscard]] std::int32_t stride() {
    if (const char* override = std::getenv("STRATUM_GOLDEN_SURFACE_STRIDE")) {
        const int value = std::atoi(override);
        if (value > 0 && value <= 32) {
            return value;
        }
    }
    return kDefaultStride;
}

/// Every sample-size floor in this file is a property of the DEFAULT stride,
/// where it detects a regression in the readback. The env var accepts up to
/// 32, at which each region contributes ONE chunk in each axis — 256 of its
/// 262144 columns — and every floor below starves on arithmetic alone. A
/// starved sample at a stride the RUNNER chose is not a finding, so it SKIPs
/// and names the stride; at the default it still fails, which is the only
/// reason the floor is worth asserting.
[[nodiscard]] bool starved(std::size_t have, std::size_t floor) {
    return have < floor && stride() > kDefaultStride;
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
    // THE SECOND ARM, and the one the Nether numbers are entitled to lean on.
    // Everything above supplies the column's surface depth exactly, which the
    // Nether run cannot do: `minecraft:surface` is itself a legacy-seeded
    // named noise there, so the depth is ENUMERATED over [-4, 11] and a bit
    // survives only where all sixteen assignments agree. That is a strictly
    // weaker decode through a different path of the same walker, and a
    // control that never took it was validating code the measurement does not
    // run.
    std::map<std::string, ControlArm> enumeratedRecovery;
    std::map<std::string, ControlArm> enumeratedNegative;
    std::size_t regions = 0;
    std::size_t replayed = 0;
    std::size_t replayAgreed = 0;
    std::size_t unexplained = 0;
    std::size_t enumeratedUnexplained = 0;

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

        // The same regions, the same decoder, the same replay gate — and the
        // surface depth left out of `Known`, which is what makes the walker
        // enumerate it. `surfaceSecondary` and `preliminarySurface` stay
        // supplied: they are modern here and are not what this arm is about.
        Walk enumerated;
        enumerated.tallies.assign(graph.conditionCount(), {});
        enumerated.fields.assign(graph.conditionCount(), {});
        enumerated.keepFields = true;
        legacy_goldens::walkRegion(
            region, overworld, decoder, stride(), true, replay,
            [&](std::int32_t x, std::int32_t z) {
                Known known;
                known.surfaceSecondary =
                    secondary.sample(static_cast<double>(x), 0.0, static_cast<double>(z));
                known.preliminarySurface = preliminary.at(x, z);
                return known;
            },
            enumerated);
        legacy_goldens::scoreAgainstRegistry(graph, decoder, noises, enumerated,
                                             enumeratedRecovery);
        legacy_goldens::scoreAgainstRegistry(graph, decoder, wrong, enumerated, enumeratedNegative);
        enumeratedUnexplained += enumerated.stats.unexplained;
        ++regions;
    }

    if (regions == 0) {
        SKIP("no golden overworld regions under " << STRATUM_FIXTURES_DIR
                                                  << " — generate them with tools/fetch-vanilla");
    }

    struct Pooled {
        std::size_t bits = 0;
        std::size_t agreed = 0;
        std::size_t majority = 0;
        std::size_t wrongAgreed = 0;
    };

    const auto pool = [](const std::map<std::string, ControlArm>& arms,
                         std::map<std::string, ControlArm>& wrongArms) {
        Pooled total;
        for (const auto& [name, arm] : arms) {
            total.bits += arm.bits;
            total.agreed += arm.agreed;
            total.majority += arm.majority;
            total.wrongAgreed += wrongArms[name].agreed;
        }
        return total;
    };
    const Pooled supplied = pool(recovery, negative);
    const Pooled enumeratedTotal = pool(enumeratedRecovery, enumeratedNegative);
    const std::size_t wrongAgreed = supplied.wrongAgreed;
    const std::size_t enumeratedWrongAgreed = enumeratedTotal.wrongAgreed;
    INFO("regions " << regions << ", replay " << replayAgreed << "/" << replayed
                    << " | SUPPLIED depth: bits " << supplied.bits << ", recovery "
                    << supplied.agreed << ", majority " << supplied.majority << ", seed+1 "
                    << wrongAgreed << " | ENUMERATED depth: bits " << enumeratedTotal.bits
                    << ", recovery " << enumeratedTotal.agreed << ", majority "
                    << enumeratedTotal.majority << ", seed+1 " << enumeratedWrongAgreed);

    // The reconstruction is checked independently of the decoder.
    CHECK(percent(replayAgreed, replayed) > 99.0);
    // On a gated position the tree explains the block by construction; a
    // position it does not is a decoder bug, not a tolerance. Both arms: the
    // enumerated walk visits strictly more assignments, so a position it
    // cannot explain is a decoder bug the supplied-depth arm was hiding.
    CHECK(unexplained == 0);
    CHECK(enumeratedUnexplained == 0);

    if (starved(supplied.bits, 201) || starved(enumeratedTotal.bits, 201)) {
        SKIP("stride " << stride() << " leaves only " << supplied.bits << " supplied-depth and "
                       << enumeratedTotal.bits
                       << " enumerated-depth bits; the floor of 200 is calibrated for the "
                          "default stride "
                       << kDefaultStride << ". Unset STRATUM_GOLDEN_SURFACE_STRIDE to assert it.");
    }
    // Enough bits that 100% is a statement rather than an accident.
    REQUIRE(supplied.bits > 200);
    // THE CONTROL. A known-correct seeding, through this decoder, on the
    // server's own regions.
    CHECK(supplied.agreed == supplied.bits);
    // AND THE NEGATIVE ARM. The wrong seeding must sit at the trivial
    // predictor — these bits are lopsided, so "78%" only means anything
    // beside the 78% a constant answer already scores.
    CHECK(percent(wrongAgreed, supplied.bits) <= percent(supplied.majority, supplied.bits) + 1.0);

    // THE ENUMERATED ARM, held to exactly the same bar. Fewer bits survive —
    // a bit has to be determined for every depth in [-4, 11] — and that is
    // the point: this is the decode the Nether run actually performs, and
    // until it was run here every control number in SPEC §11 described a path
    // the measurement never took.
    REQUIRE(enumeratedTotal.bits > 200);
    // The INVARIANT, true at any stride: enumerating explores a superset of
    // the assignments the supplied depth explores, and a bit survives only
    // where every assignment agrees, so the enumerated arm can lose bits and
    // can never gain one. A violation would mean the two arms are not running
    // the same walk.
    CHECK(enumeratedTotal.bits <= supplied.bits);
    // That it actually loses some is a property of a sample large enough to
    // contain a column whose depth matters. At stride 32 — 232 bits, 16 of
    // each region's 1024 chunks — the two arms coincide, which is thin data
    // rather than a regression, so the strict form is asserted only where the
    // sample supports it.
    if (stride() <= kDefaultStride) {
        CHECK(enumeratedTotal.bits < supplied.bits);
    }
    CHECK(enumeratedTotal.agreed == enumeratedTotal.bits);
    CHECK(percent(enumeratedWrongAgreed, enumeratedTotal.bits) <=
          percent(enumeratedTotal.majority, enumeratedTotal.bits) + 1.0);
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

    // WHICH BASES THE FIXTURES REFUTE ON THEIR OWN, pinned here because the
    // header claims it. A base that keeps a seed's top sixteen bits predicts a
    // different noise field for 0 than for Long.MIN_VALUE, and for -1 than for
    // Long.MAX_VALUE; the goldens show the SAME field for each pair. Whichever
    // bases separate a pair are refuted before the scan runs at all.
    {
        constexpr std::int64_t kMin = -9223372036854775807LL - 1;
        constexpr std::int64_t kMax = 9223372036854775807LL;
        std::size_t separating = 0;
        for (std::size_t base = 0; base < legacy_goldens::kBases; ++base) {
            const auto which = static_cast<legacy_goldens::Base>(base);
            const bool splitsZero =
                legacy_goldens::baseFor(which, 0) != legacy_goldens::baseFor(which, kMin);
            const bool splitsOnes =
                legacy_goldens::baseFor(which, -1) != legacy_goldens::baseFor(which, kMax);
            // A base either sees the top bits or it does not; it cannot see
            // them for one pair and not the other.
            CHECK(splitsZero == splitsOnes);
            separating += static_cast<std::size_t>(splitsZero ? 1 : 0);
        }
        INFO("bases separating the duplicate pairs: " << separating << " of "
                                                      << legacy_goldens::kBases);
        CHECK(separating == 2);
        // And they really are the two the header names.
        CHECK(legacy_goldens::baseFor(legacy_goldens::Base::WorldSeed, 0) !=
              legacy_goldens::baseFor(legacy_goldens::Base::WorldSeed, kMin));
        CHECK(legacy_goldens::baseFor(legacy_goldens::Base::XoroLo, 0) !=
              legacy_goldens::baseFor(legacy_goldens::Base::XoroLo, kMin));
        CHECK(legacy_goldens::baseFor(legacy_goldens::Base::LcgLong, 0) ==
              legacy_goldens::baseFor(legacy_goldens::Base::LcgLong, kMin));
        CHECK(legacy_goldens::baseFor(legacy_goldens::Base::Scrambled, 0) ==
              legacy_goldens::baseFor(legacy_goldens::Base::Scrambled, kMin));
        // And the seed list really does contain those two pairs: eight region
        // files, six worlds.
        const legacy_goldens::WorldCount counted = legacy_goldens::distinctWorlds(
            std::vector<std::int64_t>{kGoldenSeeds.begin(), kGoldenSeeds.end()});
        CHECK(counted.regions == 8);
        CHECK(counted.worlds == 6);
    }

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
    /// (F,T): soul below -0.012, gravel at or above it. THE cell — impossible
    /// under one field, expected under the tree.
    std::size_t impossibleUnderIdentity = 0;
    /// (T,F) and (T,T). The tree forbids these too — see this case's header —
    /// so they are a floor on the decoder's own error rate and say nothing
    /// about seeding.
    std::size_t impossibleUnderTree = 0;
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
            if (a == 0 && b == 1) {
                ++impossibleUnderIdentity;
            } else if (a == 1) {
                ++impossibleUnderTree;
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
                    << ", columns deciding both " << both << ", (F,T) " << impossibleUnderIdentity
                    << ", (T,*) " << impossibleUnderTree);

    if (starved(positions, 100001) || starved(both, 51) ||
        starved(netherStateSelectorColumns, 10001)) {
        SKIP("stride " << stride() << " reaches only " << positions << " positions, " << both
                       << " columns deciding both noises and " << netherStateSelectorColumns
                       << " nether_state_selector columns; these floors are calibrated for the "
                          "default stride "
                       << kDefaultStride
                       << ". Unset STRATUM_GOLDEN_SURFACE_STRIDE to assert them.");
    }
    // The readback has to reach the Nether's surface at all.
    REQUIRE(positions > 100000);
    // The decoder explains all but a small tail. That tail is not waved at:
    // vanilla's `hole` branch replaces a solid block with LAVA below y = 32,
    // which a post-rule region cannot undo, and those columns' stone-depth
    // runs are then one short.
    CHECK(percent(unexplained, positions) < 1.0);

    // THE TEST, and it is (F,T) alone. Soul below -0.012 and gravel at or
    // above it: under ONE field impossible, because both conditions test the
    // same value at the same (x, 0, z) against the same threshold whichever
    // branch reached them; under the TREE the expected cell, since a column
    // whose soul bit is false is exactly the one child 1 gets to ask about
    // gravel. (T,F) and (T,T) do NOT belong in this numerator — the tree
    // forbids them too — and pooling them in would count the decoder's own
    // error as evidence.
    REQUIRE(both > 50);
    CHECK(impossibleUnderIdentity > 0);
    CHECK(percent(impossibleUnderIdentity, both) > 10.0);

    // THE ERROR FLOOR, asserted rather than mentioned. (T,*) is impossible
    // under the tree, so every such column is the decoder or the
    // reconstruction being wrong. A FLOOR and not a measurement: an error
    // landing in (F,F) or (F,T) leaves no trace here. It has to stay small
    // beside the cell the claim rests on.
    CHECK(percent(impossibleUnderTree, both) < 5.0);
    CHECK(impossibleUnderTree * 20 < impossibleUnderIdentity);

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
    if (starved(columns.size(), 5001)) {
        SKIP("stride " << stride() << " decides only " << columns.size() << " columns for "
                       << target << "; the floor of 5000 is calibrated for the default stride "
                       << kDefaultStride << ". Unset STRATUM_GOLDEN_SURFACE_STRIDE to assert it.");
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
    const auto sweep = [&]() {
        std::size_t best = 0;
        std::size_t bestRule = 0;
        std::size_t bestBlock = 0;
        std::size_t runnerUp = 0;
        for (std::size_t ruleIndex = 0; ruleIndex < legacy_goldens::kSeedRules; ++ruleIndex) {
            for (std::size_t offset = 0; offset < 2; ++offset) {
                const std::size_t agreed = score(ruleIndex, offset);
                if (agreed > best) {
                    runnerUp = best;
                    best = agreed;
                    bestRule = ruleIndex;
                    bestBlock = offset;
                } else if (agreed > runnerUp) {
                    runnerUp = agreed;
                }
            }
        }
        struct Result {
            std::size_t best;
            std::size_t rule;
            std::size_t block;
            std::size_t runnerUp;
        };
        return Result{.best = best, .rule = bestRule, .block = bestBlock, .runnerUp = runnerUp};
    };

    const auto server = sweep();
    INFO("best of 1800 candidates on the SERVER's bits: rule "
         << server.rule << " block " << server.block << " at " << server.best << "/"
         << columns.size());
    CHECK(percent(server.best, columns.size()) < 80.0);

    // AND THE CALIBRATION, without which the line above is not a measurement.
    // Plant a candidate's own bits in place of the server's and rerun THE
    // IDENTICAL 1800-candidate sweep. Scoring the planted rule alone would
    // only show that the scorer recovers bits it just wrote — near-tautology
    // once the plant replaced them. What has to hold is RANK: the planted
    // candidate has to come first in the same sweep that found nothing on the
    // server's bits, and to beat the runner-up by a margin no wrong rule in
    // this space reaches. That is the property "no survivor" actually leans
    // on.
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
    const std::size_t planted = score(kPlantRule, kPlantBlock);
    CHECK(planted == columns.size());

    const auto recovered = sweep();
    INFO("after planting rule " << kPlantRule << " block " << kPlantBlock
                                << ": the sweep's best is "
                                << "rule " << recovered.rule << " block " << recovered.block
                                << " at " << recovered.best << "/" << columns.size()
                                << ", runner-up " << recovered.runnerUp);
    // RANK 1, and it is the planted candidate itself that holds it.
    CHECK(recovered.rule == kPlantRule);
    CHECK(recovered.block == kPlantBlock);
    CHECK(recovered.best == columns.size());
    // And by a margin: the best WRONG rule in the same sweep stays far below,
    // so rank 1 is not a photo finish that noise could have produced.
    CHECK(recovered.runnerUp < recovered.best);
    CHECK(percent(recovered.runnerUp, columns.size()) < 80.0);
}
