// Stratum — the legacy Nether's terrain chain, against vanilla's own regions.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// This is the first legacy dimension this build generates at all, and it is
// generated with an EMPTY noise registry. That is not a shortcut around
// SPEC §11's open question — it is what the measurement in
// vanilla_legacy_named_noises_test.cpp says is possible: the nether's
// `final_density` reaches no named noise, so there is no identifier to turn
// into a Java LCG seed anywhere in its terrain chain. What it does reach is
// `minecraft:old_blended_noise`, whose legacy seeding IS settled — the world
// seed handed straight to the LCG, `BlendedNoise::legacyFromWorldSeed`.
//
// WHAT IS COMPARED, AND WHAT IS DELIBERATELY NOT.
//
// The goldens are whole vanilla-generated regions, so they carry more than
// the terrain chain. Measured, not assumed: across all eight golden nether
// regions (1024 chunks each) the block names that occur are exactly eleven —
//
//   air  lava  netherrack  bedrock  crimson_nylium  warped_nylium
//   soul_sand  soul_soil  gravel  blackstone  basalt
//
// — and no glowstone, quartz, magma or fire among them, which is how these
// fixtures show they were generated with tools/fetch-vanilla's terrain-only
// datapack: carvers and features are Tier B (SPEC §7) and were removed.
// So what sits between this build's filler output and the golden is the
// SURFACE RULE, and nothing else.
//
// The nether's surface rule needs EIGHT named noises, not the six its own
// fields spell: netherrack, nether_wart, soul_sand_layer, gravel_layer,
// patch and nether_state_selector are written in the tree, and
// `minecraft:surface` and `minecraft:surface_secondary` are needed wherever
// a depth is read though they appear in no field of it
// (`surface::requiredNoises`). Its biome selection needs temperature and
// vegetation on top of that. All of them are refused under a legacy source.
// So this build cannot reproduce WHICH solid a position holds. It can
// reproduce WHETHER the position is solid, and that is the terrain chain.
//
// AND THE SURFACE RULES ARE REFUSED FOR A SECOND REASON NOW, which is worth
// stating here because it is what makes the exclusion below permanent rather
// than pending: the tree's two `vertical_gradient` conditions draw from the
// DIMENSION'S declared random source, and under a legacy source that
// derivation is measured to agree with vanilla's own bedrock at chance
// (vanilla_legacy_gradient_gap_test.cpp). Solving the named-noise seeding
// alone would not make this tree runnable.
//
// Hence the comparison is over a CLASS, not a block name:
//
//   SOLID     the nine solid names above          <- this build: netherrack
//   LAVA      minecraft:lava                      <- this build: lava
//   AIR       minecraft:air                       <- this build: air
//
// and a vanilla name outside those eleven is counted separately and fails,
// rather than being folded into whichever bucket looks closest (SPEC §8).
//
// THE EXCLUSION, stated as a bound rather than waved at. Two bands of the
// nether's 128 layers are written by the surface rule REGARDLESS of density,
// so no claim about the terrain chain can be made there:
//
//   y in [0, 5)     `bedrock_floor`, a vertical_gradient to bedrock
//   y in [123, 128) `bedrock_roof`, and then the rule immediately below it,
//                   `y_above(below_top 5) -> netherrack`, which fills the
//                   rest of that band solid whatever the density says
//
// Both are read off the pinned surface rule, not remembered. The scored
// range is therefore y in [5, 123): 118 of 128 layers, 92.2% of the column.
// The excluded 10 layers are reported too, so the cost of the exclusion is
// visible rather than implied.
//
// WHAT IT MEASURES, at the default stride of 4 — every fourth chunk of each
// region, 64 chunks a seed, 512 chunks over the eight:
//
//   15465864 / 15466496 positions exact          99.99591%
//   632 disagreeing runs, longest run 1 block
//   every one of the 632 is `solid` where vanilla has `lava` — never the
//   reverse, and air is never involved
//   every one sits in y [20, 30]
//   the excluded bands, 1310720 further positions, agree whole
//
// per seed, and note the first and last rows:
//
//   0                       1933083 / 1933312   229 runs
//   1                       1933312 / 1933312     0 runs
//   -1                      1933309 / 1933312     3 runs
//   42                      1933247 / 1933312    65 runs
//   2891948927356891        1933211 / 1933312   101 runs
//   -4172144997902289642    1933310 / 1933312     2 runs
//   9223372036854775807     1933309 / 1933312     3 runs
//   -9223372036854775808    1933083 / 1933312   229 runs
//
// Seed 0 and seed Long.MIN_VALUE are identical to the position, and so are
// -1 and Long.MAX_VALUE. That is not a coincidence to be explained away:
// java.util.Random scrambles a seed as
// `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, so bit 63 is discarded and BOTH
// pairs differ only there. Eight golden regions, **SIX INDEPENDENT WORLDS**
// — the earlier wording here said seven, having spotted only the first pair
// — and every denominator below has to be read against six. That the
// colliding pairs land on the same positions is itself evidence that the
// legacy seeding really is the LCG.
//
// AND THE TRIVIAL BASELINE, because a percentage with no null beside it is
// not a result. A stub that ignores the density chain entirely and answers
// "solid" everywhere scores the fraction of the scored band vanilla fills
// with solid blocks: **9792427 / 15466496 = 63.31%**. So 99.99591% is 36.7
// points above the trivial answer, not 12 — the round that asked for this
// baseline guessed it near 88%, and the guess was wrong because the scored
// band deliberately EXCLUDES the two bedrock bands, leaving the
// cheese-and-lava interior where more than a third of positions are not
// solid.
//
// THE BOUNDARY COUNTER was also corrected here: `previous` used to be seeded
// Solid at y = 0 and then updated only inside the scored band, so the first
// scored layer was compared against an assumption rather than the block
// below it. It now tracks the whole column. The count did NOT move — 632 of
// 226702 before and after — which is itself the evidence that the
// assumption happened to hold for the Nether (y < 5 is bedrock floor). It
// was still an assumption standing where a measurement belongs.
//
// SO THE TERRAIN CHAIN IS NOT EXACT, AND THE RESIDUAL IS BOUNDED. A longest
// run of one means the solid/fluid BOUNDARY sits one block high; the shape is
// otherwise vanilla's.
//
// AND IT IS NOT A KNIFE-EDGE, which is the part that took a second
// measurement and refuted the first reading of the first. "632 in 15.4
// million, all one block, all one direction" reads like a rounding — this
// build's density a hair over zero, landing on the wrong side of an integer y
// only because the surface is nearly horizontal there. The test evaluates
// `final_density` at every one of the 632 and that is not what it finds:
//
//   margin past zero          median 0.0101367, worst 0.0344581
//   one block of y is worth   median 0.0275955, smallest 0.0204291
//   so the iso-surface is displaced by   >= 0.367 blocks at the median,
//                                        up to 1.687 at the worst
//   and the right denominator is boundaries, not positions:
//                             632 of 226702 = 0.279%
//
// Vanilla's density there is <= 0 and this build's is > 0, so the margin is a
// LOWER bound on the gap. Where the two disagree they disagree by about a
// block of terrain, not by an ulp — but they disagree at only 0.279% of the
// boundaries where they could. Sparse and large, not uniform and small. A
// uniform epsilon would have produced the opposite shape.
//
// WHAT IS THEREFORE STILL OPEN. The cause is localised and this file cannot
// name it. It does not distinguish the `interpolated` lattice over the
// nether's 4x8 cell, `blend_density` (which should be the identity in a fresh
// world), or `BlendedNoise::legacyFromWorldSeed` itself — whose probe scored
// terrain height to within HALF A BLOCK and would have passed a
// third-of-a-block error without seeing it, so "13824 of 13824" does not
// exclude this. `tools/analysis/final-density-probe.sh` bisects the server's
// own density at a point rather than reading the sign of its terrain, and is
// what would decide it; that needs a probe world and is not done here.
//
// A disagreement here is either this build's density or a surface rule
// reaching past the two bands above, which is why the confusion matrix and
// the y histogram are printed rather than a single percentage.
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Without
// them this skips.
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using stratum::chunk::Chunk;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::region::RegionFile;
using stratum::settings::LoadedSettings;
using stratum::settings::NoiseSettings;
using stratum::settings::RouterEntry;

// The eight seeds tools/fetch-vanilla generated golden regions for. Eight,
// because one world seed agreeing with an RNG-driven derivation is evidence
// that it is right once (SPEC §11, and the same reason the blended-noise
// case runs three).
constexpr std::array<std::int64_t, 8> kSeeds{
    0,
    1,
    -1,
    42,
    2891948927356891LL,
    -4172144997902289642LL,
    9223372036854775807LL,
    -9223372036854775807LL - 1,
};

// Read off nether.json's surface_rule, not remembered: sequence[0] is the
// bedrock floor over `above_bottom 0..5`, sequence[1] the bedrock roof over
// `below_top 5..0`, and sequence[2] turns the whole of that upper band to
// netherrack whatever the density under it says.
constexpr std::int32_t kScoredMinY = 5;
constexpr std::int32_t kScoredMaxY = 123; // exclusive

enum class Class : std::uint8_t { Solid, Lava, Air, Unknown };

[[nodiscard]] constexpr std::string_view className(Class value) noexcept {
    switch (value) {
        case Class::Solid:
            return "solid";
        case Class::Lava:
            return "lava";
        case Class::Air:
            return "air";
        case Class::Unknown:
            break;
    }
    return "unknown";
}

/// The eleven names that actually occur across all eight golden nether
/// regions, censused with tools/analysis/region-palette.py. Anything else is
/// Unknown on purpose: a twelfth name would mean these fixtures were made
/// differently, and guessing its class would hide that.
[[nodiscard]] Class classOfVanilla(std::string_view name) noexcept {
    if (name == "minecraft:air") {
        return Class::Air;
    }
    if (name == "minecraft:lava") {
        return Class::Lava;
    }
    if (name == "minecraft:netherrack" || name == "minecraft:bedrock" ||
        name == "minecraft:crimson_nylium" || name == "minecraft:warped_nylium" ||
        name == "minecraft:soul_sand" || name == "minecraft:soul_soil" ||
        name == "minecraft:gravel" || name == "minecraft:blackstone" ||
        name == "minecraft:basalt") {
        return Class::Solid;
    }
    return Class::Unknown;
}

/// This build's own output, which under an empty registry and no surface
/// rules is exactly the three the filler can place.
[[nodiscard]] Class classOfOurs(const ResourceLocation& name) noexcept {
    const std::string text = name.toString();
    if (text == "minecraft:air") {
        return Class::Air;
    }
    if (text == "minecraft:lava") {
        return Class::Lava;
    }
    return Class::Solid;
}

struct Tally {
    std::size_t scored = 0;
    std::size_t agree = 0;
    std::size_t agreeSolidity = 0; // solid vs not, ignoring lava/air
    std::size_t unknownVanilla = 0;
    std::map<std::string, std::size_t> confusion;
    /// Where the disagreements are, by y. A residual that sits on one layer
    /// is a rule; a residual scattered down the column is arithmetic. Telling
    /// those apart is the whole reason this is kept rather than a total.
    std::map<std::int32_t, std::size_t> disagreementByY;
    std::vector<std::string> samples;
    /// HOW FAR PAST ZERO this build's `final_density` actually is where the
    /// two disagree, and how much one block of y moves it there. Vanilla's
    /// density at such a position is <= 0 and ours is > 0, so the margin is a
    /// LOWER BOUND on the difference between the two densities — and read
    /// against the per-block step it says whether this is a knife-edge
    /// crossing or a genuinely different value. That is the difference
    /// between "the arithmetic agrees to within a rounding of the boundary"
    /// and "the legacy path computes something else", which the block-class
    /// comparison alone cannot tell apart.
    std::vector<double> margins;
    std::vector<double> perBlockSteps;
    /// Solid-to-not transitions in this build's own output within the scored
    /// band. The right denominator for a boundary error: 632 against 15.4
    /// million positions makes the residual look like noise, and against the
    /// boundaries it could possibly have moved it does not.
    std::size_t boundaries = 0;
    /// The longest unbroken run of disagreeing y in any one column, and how
    /// many columns hold such a run at all. A longest run of 1 means every
    /// disagreement is the solid/fluid boundary sitting exactly one block
    /// off — the shape is right and its height is not, which is a very
    /// different claim from the shape being wrong.
    std::size_t longestRun = 0;
    std::size_t columnsWithDisagreement = 0;
    /// THE TRIVIAL BASELINE, so that 99.99591% can be read against something
    /// rather than against 100%. A stub that ignores the density chain and
    /// calls every position Solid scores exactly this: the count of vanilla
    /// positions that ARE solid. It is the score to beat before the real
    /// number means anything, and stating it is the difference between "the
    /// terrain matches" and "the metric is easy".
    std::size_t allSolidStub = 0;
    /// A SECOND, INDEPENDENT PROPERTY, folded in from the round that measured
    /// it separately: every block where this build and vanilla differ must be
    /// one the Nether's OWN surface rule tree could have placed over what the
    /// density chain left. The surface rules are not run here — the tree
    /// names noises this build cannot seed under a legacy source, and §8
    /// refuses a tree whole — so every position the tree would have
    /// repainted still carries the density chain's block. That makes an
    /// exact-NAME comparison impossible and this bound the honest replacement
    /// for it: a disagreement the tree cannot explain is a terrain error and
    /// is reported BY NAME rather than absorbed by a tolerance.
    std::map<std::string, std::size_t> unexplained;

    void add(Class ours, Class theirs, std::int32_t x, std::int32_t y, std::int32_t z) {
        ++scored;
        if (theirs == Class::Solid) {
            ++allSolidStub;
        }
        if (theirs == Class::Unknown) {
            ++unknownVanilla;
        }
        if (ours == theirs) {
            ++agree;
        } else {
            confusion[std::string(className(ours)) + " != " + std::string(className(theirs))] += 1;
            disagreementByY[y] += 1;
            if (samples.size() < 8U) {
                samples.push_back("(" + std::to_string(x) + ", " + std::to_string(y) + ", " +
                                  std::to_string(z) + ") ours=" + std::string(className(ours)) +
                                  " vanilla=" + std::string(className(theirs)));
            }
        }
        if ((ours == Class::Solid) == (theirs == Class::Solid)) {
            ++agreeSolidity;
        }
    }
};

[[nodiscard]] std::filesystem::path findWorldgenTree() {
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path netherRegionOf(std::int64_t seed) {
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() != "r.0.0.mca") {
            continue;
        }
        const std::filesystem::path parent = entry.path().parent_path();
        if (parent.filename() != "nether") {
            continue;
        }
        if (parent.parent_path().filename() == "seed-" + std::to_string(seed)) {
            return entry.path();
        }
    }
    return {};
}

/// Every fourth chunk of the 32x32 region, so the sample is spread over the
/// whole 512x512 blocks rather than packed into one corner — the same reason
/// golden_terrain_test.cpp strides its columns. STRATUM_NETHER_STRIDE
/// overrides it for a wider local run.
[[nodiscard]] std::int32_t chunkStride() {
    if (const char* override = std::getenv("STRATUM_NETHER_STRIDE")) {
        const int value = std::atoi(override);
        if (value > 0 && value <= 32) {
            return value;
        }
    }
    return 4;
}

} // namespace

TEST_CASE("the legacy Nether's terrain matches vanilla with no named noise built",
          "[conformance][legacy][nether][terrain]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::open(tree);
    const LoadedSettings loaded = stratum::settings::loadAll(pack);
    const NoiseSettings& nether = loaded.settings.at(ResourceLocation::parse("minecraft:nether"));
    REQUIRE(nether.legacyRandomSource);
    REQUIRE_FALSE(nether.aquifersEnabled);
    REQUIRE_FALSE(nether.oreVeinsEnabled);

    // EVERY BLOCK THE NETHER'S OWN TREE CAN PLACE, read out of the resolved
    // tree rather than written down here, so that a disagreement the surface
    // rules cannot explain fails this test by name instead of hiding inside
    // a tolerance. The tree is resolved but never run — see Tally::unexplained.
    const auto netherId = ResourceLocation::parse("minecraft:nether");
    const auto netherRules = stratum::surface::RuleGraph::resolve(nether.surfaceRule, netherId);
    // And it is still out, by name: if this ever becomes buildable the
    // comparison below should become an exact-block one.
    CHECK_FALSE(stratum::surface::requiredNoises(netherRules).empty());
    std::set<std::string> placeable;
    for (stratum::surface::RuleIndex i = 0; i < netherRules.ruleCount(); ++i) {
        const auto& rule = netherRules.rule(i);
        if (rule.type == stratum::surface::RuleType::Block) {
            placeable.insert(rule.block.name.toString());
        }
    }
    REQUIRE(placeable.size() > 1U);

    const std::int32_t stride = chunkStride();
    Tally overall;
    Tally excludedBands;
    std::size_t seedsMeasured = 0;
    std::size_t chunksMeasured = 0;

    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path region = netherRegionOf(seed);
        if (region.empty()) {
            continue;
        }
        CAPTURE(seed, region.string());

        // The whole point: an EMPTY wanted set under a Legacy source. This
        // throws before the narrowing in noise_registry.cpp, and it would
        // still throw for any set that named something.
        const std::vector<ResourceLocation> nothing;
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, nothing, seed, stratum::density::RandomSource::Legacy);
        REQUIRE(noises.size() == 0);

        // No surface rules, no biome parameters: bare default block, fluid
        // and air, which is exactly the terrain chain and nothing after it.
        const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, nether);
        REQUIRE_FALSE(filler.runsSurfaceRules());

        // The same chain the filler runs, reachable point by point, so that a
        // disagreement can be asked HOW FAR past zero it is rather than only
        // which side of zero it landed on.
        const stratum::density::Interpreter interpreter(
            loaded.graph, noises,
            stratum::density::CellGeometry{.width = nether.geometry.cellWidth(),
                                           .height = nether.geometry.cellHeight()});
        const auto finalDensity = nether.router.at(RouterEntry::FinalDensity);

        const RegionFile file = RegionFile::open(region);
        Tally perSeed;

        for (std::int32_t cz = 0; cz < stratum::region::kChunksPerAxis; cz += stride) {
            for (std::int32_t cx = 0; cx < stratum::region::kChunksPerAxis; cx += stride) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const Chunk golden = Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                REQUIRE(golden.status() == "minecraft:full");

                stratum::terrain::ChunkBuffer ours(nether.geometry);
                filler.fill(golden.x(), golden.z(), ours);
                ++chunksMeasured;

                // Column-major, so that a disagreement can be measured as a
                // RUN down the column rather than only as a count of
                // positions. The distinction is the whole difference between
                // "the terrain boundary sits one block off here" and "the
                // terrain is a different shape here", and a position count
                // cannot tell them apart.
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        std::size_t run = 0;
                        // TRACKED ACROSS THE WHOLE COLUMN, not only inside
                        // the scored band. It used to be seeded Solid at y=0
                        // and then updated only where `scored` was true, so
                        // the first scored layer was compared against an
                        // ASSUMPTION about y = kScoredMinY - 1 rather than
                        // against the block actually there. For the nether
                        // that assumption happens to hold — y<5 is the
                        // bedrock floor — which is exactly why it went
                        // unnoticed; it is still an assumption standing where
                        // a measurement belongs, and it silently decided
                        // whether a boundary could be counted at the band's
                        // first layer.
                        Class previous = Class::Solid;
                        bool havePrevious = false;
                        for (std::int32_t y = 0; y < nether.geometry.height; ++y) {
                            const bool scored = y >= kScoredMinY && y < kScoredMaxY;
                            Tally& into = scored ? perSeed : excludedBands;
                            const stratum::chunk::BlockState* theirs = golden.blockAt(lx, y, lz);
                            REQUIRE(theirs != nullptr);
                            const std::string ourName = ours.at(lx, y, lz).name.toString();
                            const Class ourClass = classOfOurs(ours.at(lx, y, lz).name);
                            const Class theirClass = classOfVanilla(theirs->name);
                            into.add(ourClass, theirClass, (golden.x() * 16) + lx, y,
                                     (golden.z() * 16) + lz);
                            if (scored && ourName != theirs->name &&
                                !placeable.contains(theirs->name)) {
                                into.unexplained[theirs->name + " where this build has " +
                                                 ourName] += 1;
                            }
                            const Class before = previous;
                            const bool hadPrevious = havePrevious;
                            previous = ourClass;
                            havePrevious = true;
                            if (!scored) {
                                continue;
                            }
                            if (hadPrevious && before == Class::Solid && ourClass != Class::Solid) {
                                ++into.boundaries;
                            }
                            if (ourClass == theirClass) {
                                run = 0;
                                continue;
                            }
                            ++run;
                            perSeed.longestRun = std::max(perSeed.longestRun, run);
                            if (run == 1U) {
                                ++perSeed.columnsWithDisagreement;
                            }

                            // Only 632 of these over the whole run, so the
                            // cost of evaluating the chain again here is
                            // nothing beside the fill that found them.
                            const stratum::density::Point at{
                                .x = (golden.x() * 16) + lx, .y = y, .z = (golden.z() * 16) + lz};
                            const double here = interpreter.evaluate(finalDensity, at);
                            const double above = interpreter.evaluate(
                                finalDensity,
                                stratum::density::Point{.x = at.x, .y = y + 1, .z = at.z});
                            perSeed.margins.push_back(here);
                            perSeed.perBlockSteps.push_back(here - above);
                        }
                    }
                }
            }
        }

        REQUIRE(perSeed.scored > 0U);
        ++seedsMeasured;

        const double exact =
            100.0 * static_cast<double>(perSeed.agree) / static_cast<double>(perSeed.scored);
        WARN("seed " << seed << ": " << perSeed.agree << " / " << perSeed.scored << " = " << exact
                     << "% exact class over y in [" << kScoredMinY << ", " << kScoredMaxY << "); "
                     << perSeed.columnsWithDisagreement << " column run(s), longest "
                     << perSeed.longestRun << " block(s)");
        for (const auto& [what, count] : perSeed.confusion) {
            WARN("    " << what << ": " << count);
        }
        for (const auto& [y, count] : perSeed.disagreementByY) {
            WARN("    y = " << y << ": " << count);
        }
        for (const std::string& sample : perSeed.samples) {
            WARN("    " << sample);
        }

        // No vanilla block outside the censused eleven. A twelfth would mean
        // these fixtures are not terrain-only and every number here is about
        // something else.
        CHECK(perSeed.unknownVanilla == 0U);

        overall.boundaries += perSeed.boundaries;
        overall.margins.insert(overall.margins.end(), perSeed.margins.begin(),
                               perSeed.margins.end());
        overall.perBlockSteps.insert(overall.perBlockSteps.end(), perSeed.perBlockSteps.begin(),
                                     perSeed.perBlockSteps.end());
        overall.longestRun = std::max(overall.longestRun, perSeed.longestRun);
        overall.columnsWithDisagreement += perSeed.columnsWithDisagreement;
        overall.scored += perSeed.scored;
        overall.agree += perSeed.agree;
        overall.allSolidStub += perSeed.allSolidStub;
        for (const auto& [what, count] : perSeed.unexplained) {
            overall.unexplained[what] += count;
        }
        overall.agreeSolidity += perSeed.agreeSolidity;
        overall.unknownVanilla += perSeed.unknownVanilla;
        for (const auto& [what, count] : perSeed.confusion) {
            overall.confusion[what] += count;
        }
    }

    if (seedsMeasured == 0) {
        SKIP("no golden nether regions under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla --generate-regions --accept-eula");
    }

    WARN("nether terrain, " << seedsMeasured << " seed(s), " << chunksMeasured
                            << " chunk(s), stride " << stride << ": " << overall.agree << " / "
                            << overall.scored << " positions exact; " << overall.agreeSolidity
                            << " / " << overall.scored << " agree on solidity; excluded bands y<"
                            << kScoredMinY << " and y>=" << kScoredMaxY << " held "
                            << excludedBands.scored << " further positions, " << excludedBands.agree
                            << " of which agree anyway; " << overall.columnsWithDisagreement
                            << " disagreeing run(s), longest " << overall.longestRun
                            << " block(s)");

    // HOW FAR PAST ZERO. Vanilla's density at a disagreeing position is
    // <= 0 and this build's is > 0, so the margin below is a lower bound on
    // the gap between the two densities. Read against the per-block step it
    // says whether the boundary is a knife-edge crossing or a different
    // surface.
    if (!overall.margins.empty()) {
        auto sortedMargins = overall.margins;
        auto sortedSteps = overall.perBlockSteps;
        std::sort(sortedMargins.begin(), sortedMargins.end());
        std::sort(sortedSteps.begin(), sortedSteps.end());
        const double worstMargin = sortedMargins.back();
        const double medianMargin = sortedMargins[sortedMargins.size() / 2];
        const double smallestStep = sortedSteps.front();
        const double medianStep = sortedSteps[sortedSteps.size() / 2];
        WARN("margin past zero over "
             << sortedMargins.size() << " disagreement(s): median " << medianMargin << ", worst "
             << worstMargin << "; one block of y moves final_density by " << medianStep
             << " (smallest " << smallestStep << "), so the iso-surface is displaced by at least "
             << medianMargin / medianStep << " block(s) at the median and up to "
             << worstMargin / smallestStep << "; " << overall.columnsWithDisagreement << " of "
             << overall.boundaries << " solid-to-fluid boundaries in the scored band");

        // Every margin is positive by construction — this build called the
        // position solid — so a non-positive one would mean the filler and
        // the interpreter disagree with each other, which is a different bug
        // entirely and worth catching here.
        CHECK(sortedMargins.front() > 0.0);
        CHECK(smallestStep > 0.0);

        // A HYPOTHESIS THIS REFUTED, kept because the refutation is the
        // result. The obvious reading of "632 positions in 15.4 million, all
        // one block, all one direction" is a knife-edge: this build's density
        // a hair over zero, the iso-surface in the same place to within a
        // rounding, landing on the wrong side of an integer y only because
        // the surface is nearly horizontal. That would have made the residual
        // a floating-point epsilon of the kind the overworld's own chase
        // already found once (SPEC §11).
        //
        // It is not that. The margin is a THIRD OF A BLOCK at the median
        // (0.0101 against a per-block step of 0.0276) and up to 1.69 blocks
        // at the worst. Vanilla's density at these positions is <= 0 and this
        // build's is > 0, so the margin is a LOWER bound on the gap: where
        // the two disagree they disagree by an amount comparable to a whole
        // block of terrain, not by an ulp.
        //
        // WHAT THAT LEAVES OPEN, stated precisely rather than resolved. The
        // residual is sparse — 632 of the solid-to-fluid boundaries in the
        // band, not 632 of everything — and where it bites it is large. A
        // uniform small bias would have produced the opposite shape: many
        // more disagreements, each a hair over zero. So the cause is
        // localised, and this file cannot name it. Three candidates it does
        // NOT distinguish: the `interpolated` cell lattice over the nether's
        // 4x8 cell; `blend_density`, which should be the identity in a fresh
        // world; and `BlendedNoise::legacyFromWorldSeed` itself, whose probe
        // accepted a column at half a block and would therefore have passed a
        // third-of-a-block error without seeing it.
        //
        // The tool for the next round is `tools/analysis/final-density-probe.
        // sh`, which bisects the SERVER's own density at a point instead of
        // reading the sign of its terrain — the same tool that identified the
        // overworld's missing epsilon. Run at these coordinates it would say
        // which of the three it is. That needs a probe world and is not done
        // here.
        //
        // Asserted as the measured bound, so that a fix moves it and a
        // regression moves it further:
        CHECK(medianMargin < medianStep);
        CHECK(worstMargin > 0.01 * smallestStep);
    }

    // Eight seeds, not one. A single seed is how a coincidence gets mistaken
    // for a derivation — though note that FOUR of these eight are two
    // Nethers: java.util.Random scrambles its seed as
    // `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, so bit 63 is discarded, and
    // BOTH 0 / Long.MIN_VALUE and -1 / Long.MAX_VALUE differ only there.
    // Their rows above are identical to the position, which is itself a
    // check that the legacy seeding really is the LCG. So this is eight
    // golden regions over SIX INDEPENDENT WORLDS, and that is the number
    // every denominator in this file has to be read against. The earlier
    // wording here said seven, having spotted only the first pair.
    //
    // Asserted rather than asserted-in-prose: the two collisions are a fact
    // about java.util.Random, so they are computed here from the same
    // scramble the LCG uses.
    {
        constexpr std::uint64_t kMultiplierMask = (1ULL << 48U) - 1U;
        constexpr std::uint64_t kScramble = 0x5DEECE66DULL;
        std::set<std::uint64_t> distinct;
        for (const std::int64_t seed : kSeeds) {
            distinct.insert((static_cast<std::uint64_t>(seed) ^ kScramble) & kMultiplierMask);
        }
        CHECK(distinct.size() == 6U);
    }
    CHECK(seedsMeasured == kSeeds.size());
    CHECK(overall.unknownVanilla == 0U);

    // THE TRIVIAL BASELINE, MEASURED AND STATED. A percentage with no null
    // beside it is not a result: this band is mostly netherrack, so a stub
    // that answers "solid" everywhere already scores well. The real number
    // has to be read as the distance from here to 100%, not from 0%.
    {
        const double stub =
            100.0 * static_cast<double>(overall.allSolidStub) / static_cast<double>(overall.scored);
        const double measured =
            100.0 * static_cast<double>(overall.agree) / static_cast<double>(overall.scored);
        WARN("all-solid stub baseline: " << overall.allSolidStub << " / " << overall.scored << " = "
                                         << stub << "%, against this build's " << measured << "%");
        // MEASURED AT 63.31%, and the round that asked for this baseline
        // guessed it at roughly 88%. It is not: the scored band deliberately
        // EXCLUDES the two bedrock bands, which are the solid-by-definition
        // part of the column, and what is left is the cheese-and-lava
        // interior where more than a third of the positions are not solid.
        // So the metric is a good deal less easy than it looked, and the
        // 99.99591% is 36.7 points above the trivial answer rather than 12.
        CHECK(stub > 60.0);
        CHECK(stub < 70.0);
        CHECK(measured > stub);
        // Of the positions the stub gets wrong, this build gets all but a
        // handful right — which is the claim the baseline was needed for.
        const std::size_t stubWrong = overall.scored - overall.allSolidStub;
        const std::size_t oursWrong = overall.scored - overall.agree;
        CHECK(oursWrong * 1000U < stubWrong);
        // And pinned at the default stride, beside the other numbers, so a
        // change to the scored band moves it visibly rather than quietly
        // re-scaling what the headline is read against.
        if (stride == 4) {
            CHECK(overall.allSolidStub == 9792427U);
        }
    }

    // THE SHAPE OF THE RESIDUAL, asserted rather than described, because the
    // description is the result.
    //
    // 1. Every disagreement is an isolated single block down its column. Not
    //    "mostly"; the longest unbroken run over 15.4 million positions is
    //    one. The solid/fluid boundary sits one block off, and the terrain's
    //    shape is otherwise reproduced.
    CHECK(overall.longestRun <= 1U);

    // 2. And it is one-directional: this build is solid where vanilla is
    //    lava, never the reverse, and air is never involved at all. A
    //    two-directional residual would be scatter; a one-directional one is
    //    a bias, and saying which it is costs one assertion.
    CHECK(overall.confusion.size() <= 1U);
    for (const auto& [what, count] : overall.confusion) {
        INFO(count);
        CHECK(what == "solid != lava");
    }

    // 2b. AND THE SECOND PROPERTY: every differing block is one the Nether's
    //     own rule tree can place. This is a stronger statement than the
    //     class bound above and is what rules out "the terrain is wrong here"
    //     as an explanation for the positions the missing surface rules
    //     leave unpainted. The category-crossing ones are the tree's single
    //     `minecraft:lava` placement under a `hole` condition — a real,
    //     named exception, counted by the same rule as the rest rather than
    //     tolerated.
    for (const auto& [what, count] : overall.unexplained) {
        INFO(what << " x " << count);
        CHECK(count == 0U);
    }
    CHECK(overall.unexplained.empty());

    // 3. The number itself, pinned at the default stride with its
    //    denominator, so that a regression moves it rather than hiding under
    //    a threshold. 632 of 15466496 is 4.09e-5 — and every one of the 632
    //    sits in y [20, 30], where the nether's `y_clamped_gradient(-8 -> 24)`
    //    has scaled the noise term nearly to nothing and the iso-surface is
    //    nearly horizontal, which is where a boundary is most sensitive to a
    //    small difference in density.
    //
    // WHAT THIS DOES NOT SETTLE. Whether that difference is specific to the
    // LEGACY blended-noise path or is a shared arithmetic residual too small
    // for the overworld's heightmap statistic to have shown is NOT decided
    // here, and this test cannot decide it: the overworld's own comparison
    // scores 4096 column HEIGHTS, not 15 million block classes, and its
    // aquifer gap sits in front of anything finer. What is settled is the
    // bound: one block, one direction, 4.09e-5.
    if (stride == 4) {
        CHECK(overall.scored == 15466496U);
        CHECK(overall.agree == 15465864U);
        CHECK(overall.columnsWithDisagreement == 632U);
    }

    // The excluded bands are excluded on principle, not because they
    // disagree: at the CLASS level they come out whole, since this build
    // fills them solid and vanilla's bedrock floor, bedrock roof and
    // `y_above -> netherrack` are solid too. The exclusion would matter to a
    // comparison of block NAMES, which is the one the missing named noises
    // make impossible.
    CHECK(excludedBands.agree == excludedBands.scored);
}
