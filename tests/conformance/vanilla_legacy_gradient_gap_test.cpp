// Stratum — what a legacy random source does to `vertical_gradient`.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// THE HOLE THIS PINNED. `NoiseRegistry::create` was narrowed to refuse a
// `legacy_random_source` dimension only when it names a worldgen/noise. That
// predicate is about NOISES, and a legacy dimension draws on its declared
// random source for three things that are not noises: a
// `minecraft:vertical_gradient`'s `random_name`, the aquifer lattice's centre
// jitter, and the ore-vein source. None of the three passes through the
// registry's `wanted` list. This build once derived all three with
// Xoroshiro128++ and refused them by name under a legacy source; all three
// are derived from the declared source now, each measured (SPEC §11), the
// vein source last ("Ore veins under the legacy source").
//
// For `vertical_gradient` the Xoroshiro coin is MEASURED WRONG rather than
// merely unverified, and this file is the measurement. Vanilla's own Nether writes
// its bedrock floor and roof with two `vertical_gradient` conditions, so the
// golden Nether regions already on disk are a labelled outcome at every
// block of the two probabilistic bands — exactly the shape
// vanilla_vertical_gradient_test.cpp uses on the modern probe worlds, applied
// to a dimension whose random source is the Java LCG.
//
// WHAT MAKES THE NUMBER READABLE. In the band between a gradient's two
// anchors the outcome is a weighted coin, so "agrees at chance" is a
// computable number, not a hand-wave: a predictor drawing independently from
// the same per-level probability agrees at sum p^2 + (1-p)^2 over the band.
// The ladder is 80/60/40/20% over the Nether's four probabilistic levels
// (and 100% at the certain one, which is excluded because it carries no
// information), so chance is 0.68 + 0.52 + 0.52 + 0.68 = 2.40 levels of
// 4 — 39322 of 65536 over the sample below. The Xoroshiro prediction lands
// there.
//
// THE CONTROL IS IN THIS FILE ON PURPOSE. The same code, the same sample
// size, the same seeds, run against the modern OVERWORLD's bedrock floor,
// is exact. So what the Nether number measures is the SEEDING under a legacy
// source and not this build's gradient, its region reader, or its anchor
// resolution — all three of which the control exercises identically.
//
// WHAT THE LEGACY DERIVATION IS, since. The aquifer lattice draws from the
// same primitive, and a constructed probe (tools/analysis/legacy-aquifer-
// probe.sh, SPEC §11) measured it there: two java.util.Random forks around
// String.hashCode of the name, the position mix XORed in
// (rng::legacyPositionalSourceFor). That primitive, salted with each
// gradient's own random_name and drawing one nextFloat, is scored here too,
// as an out-of-sample check — nothing about the bedrock went into finding
// it — and it is exact. The executor runs it wherever its registry declares
// the legacy source, so the executor ITSELF is scored here as well, on the
// golden Nether's two bedrock rules, and so is a doctored legacy pack driven
// through the public CompiledDimension path. The ore-vein source has no
// oracle at all: every vanilla legacy dimension has the flag off.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/temp_path.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using Catch::Matchers::ContainsSubstring;

namespace {

using stratum::data::ResourceLocation;
using stratum::settings::NoiseSettings;

/// Four seeds, and they are four distinct worlds: 0 and LONG_MIN collide
/// under `java.util.Random`'s 48-bit scramble and LONG_MAX collides with -1,
/// so a seed list that wants four independent answers cannot hold both of
/// either pair. 0, 1, -1 and 42 hold no colliding pair.
constexpr std::array<std::int64_t, 4> kSeeds = {0, 1, -1, 42};

/// 8x8 chunks of r.0.0 — 16384 columns, which over a four-level band is
/// 65536 labelled positions per gradient per seed. Large enough that the
/// chance band is narrow (its standard deviation is about 125 positions) and
/// small enough that eight regions are read in seconds.
constexpr std::int32_t kChunksPerAxis = 8;
constexpr std::size_t kColumns = 16384;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Gradient {
    std::string randomName;
    std::int32_t trueAtAndBelow = 0;
    std::int32_t falseAtAndAbove = 0;
    /// Whether the tree places its block where the gradient does NOT fire —
    /// read out of the resolved tree (a `not` naming this condition) rather
    /// than assumed from the anchors, because the Nether's roof is exactly
    /// the inverted case and getting it backwards would turn an agreement
    /// into a disagreement and read as evidence.
    bool inverted = false;
};

/// The block a rule guarded by condition @p guard places directly, if it
/// places one directly. Empty when the rule under it is a sequence or another
/// condition, which is what makes a gradient unreadable from the blocks.
[[nodiscard]] std::string directBlockUnder(const stratum::surface::RuleGraph& rules,
                                           const stratum::surface::ConditionIndex guard) {
    for (stratum::surface::RuleIndex i = 0; i < rules.ruleCount(); ++i) {
        const auto& rule = rules.rule(i);
        if (rule.type != stratum::surface::RuleType::Condition || rule.condition != guard) {
            continue;
        }
        const auto& inner = rules.rule(rule.thenRun);
        if (inner.type == stratum::surface::RuleType::Block) {
            return inner.block.name.toString();
        }
    }
    return {};
}

/// Every `vertical_gradient` in @p rules that PLACES BEDROCK DIRECTLY, with
/// its anchors resolved against @p geometry and its polarity read off the
/// tree.
///
/// WHY ONLY THOSE, and why the restriction is structural rather than a
/// convenience. Scoring a gradient against the server's blocks needs the
/// gradient's outcome to BE the block: one unconditioned rule, one block,
/// nothing else deciding. The bedrock floor and roof are exactly that in
/// every dimension that has them. The overworld's `minecraft:deepslate`
/// gradient is not — it sits under further conditions, so a position where
/// it fired can still hold something else, and scoring it against "is this
/// deepslate" would measure the rest of the tree rather than the gradient.
/// That exclusion is a property of the tree's SHAPE, decided before any
/// block is read, so it cannot select for a favourable answer.
[[nodiscard]] std::vector<Gradient>
bedrockGradientsOf(const stratum::surface::RuleGraph& rules,
                   const stratum::settings::NoiseGeometry& geometry) {
    std::vector<Gradient> out;
    for (stratum::surface::ConditionIndex i = 0; i < rules.conditionCount(); ++i) {
        const auto& condition = rules.condition(i);
        if (condition.type != stratum::surface::ConditionType::VerticalGradient) {
            continue;
        }
        bool inverted = false;
        std::string placed = directBlockUnder(rules, i);
        for (stratum::surface::ConditionIndex j = 0; j < rules.conditionCount(); ++j) {
            const auto& other = rules.condition(j);
            if (other.type == stratum::surface::ConditionType::Not && other.invert == i) {
                inverted = true;
                if (placed.empty()) {
                    placed = directBlockUnder(rules, j);
                }
            }
        }
        if (placed != "minecraft:bedrock") {
            continue;
        }
        out.push_back(Gradient{.randomName = condition.randomName,
                               .trueAtAndBelow = condition.trueAtAndBelow.resolve(geometry),
                               .falseAtAndAbove = condition.falseAtAndAbove.resolve(geometry),
                               .inverted = inverted});
    }
    return out;
}

struct BandScore {
    std::size_t scored = 0;
    std::size_t agree = 0;
    /// Bedrock in the golden, per level of the band, so the per-level ladder
    /// can be asserted rather than assumed from the anchors.
    std::map<std::int32_t, std::size_t> bedrockByY;
    /// And the two CERTAIN rungs, which the scored band deliberately excludes
    /// because they carry no information about the random source. They carry
    /// everything about the ANCHORS, though: if the level the schema says is
    /// certain is not, the band being scored is not the band the anchors
    /// name and every number here is about something else. 100% and 0%.
    std::size_t certainTrue = 0;
    std::size_t certainFalse = 0;
    std::size_t certainColumns = 0;
};

/// Scores a prediction of bedrock against the golden region's own bedrock,
/// over the gradient's PROBABILISTIC levels only. @p predictsBedrock is asked
/// at absolute (x, y, z).
template<typename PredictsBedrock>
[[nodiscard]] BandScore scoreBand(const std::filesystem::path& region, const Gradient& gradient,
                                  PredictsBedrock&& predictsBedrock) {
    const auto file = stratum::region::RegionFile::open(region);
    BandScore score;
    for (std::int32_t cz = 0; cz < kChunksPerAxis; ++cz) {
        for (std::int32_t cx = 0; cx < kChunksPerAxis; ++cx) {
            REQUIRE(file.hasChunk(cx, cz));
            const auto golden =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    // Strictly between the anchors: at and below the first
                    // the outcome is certain, at and above the second it is
                    // impossible, and a level with no information in it would
                    // only inflate the agreement.
                    for (std::int32_t y = gradient.trueAtAndBelow + 1; y < gradient.falseAtAndAbove;
                         ++y) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        const auto* block = golden.blockAt(lx, y, lz);
                        REQUIRE(block != nullptr);
                        const bool bedrock = block->name == "minecraft:bedrock";
                        const bool predicted = predictsBedrock(x, y, z);
                        ++score.scored;
                        score.agree += static_cast<std::size_t>(predicted == bedrock);
                        score.bedrockByY[y] += static_cast<std::size_t>(bedrock);
                    }

                    // The two rungs the band excludes. Read at the anchors
                    // themselves: at `trueAtAndBelow` the gradient is certain
                    // and at `falseAtAndAbove` it is impossible, so the
                    // golden must be all bedrock at one and none at the
                    // other (reversed where the gradient is inverted).
                    const auto* atTrue = golden.blockAt(lx, gradient.trueAtAndBelow, lz);
                    const auto* atFalse = golden.blockAt(lx, gradient.falseAtAndAbove, lz);
                    if (atTrue != nullptr && atFalse != nullptr) {
                        ++score.certainColumns;
                        const bool trueRung = atTrue->name == "minecraft:bedrock";
                        const bool falseRung = atFalse->name == "minecraft:bedrock";
                        score.certainTrue +=
                            static_cast<std::size_t>(gradient.inverted ? !trueRung : trueRung);
                        score.certainFalse +=
                            static_cast<std::size_t>(gradient.inverted ? !falseRung : falseRung);
                    }
                }
            }
        }
    }
    return score;
}

/// Which positional coin a score draws.
enum class Coin : std::uint8_t {
    /// The modern derivation, which the executor runs under a modern source.
    Xoroshiro,
    /// The legacy derivation measured on the aquifer's lattice, which the
    /// executor runs under a legacy one.
    Legacy,
};

/// Scores one coin, drawn directly, read through the gradient's polarity.
[[nodiscard]] BandScore scoreGradient(const std::filesystem::path& region,
                                      const std::int64_t worldSeed, const Gradient& gradient,
                                      const Coin coin = Coin::Xoroshiro) {
    const auto source = stratum::rng::positionalSourceFor(worldSeed, gradient.randomName);
    const auto legacy = stratum::rng::legacyPositionalSourceFor(worldSeed, gradient.randomName);
    return scoreBand(
        region, gradient, [&](const std::int32_t x, const std::int32_t y, const std::int32_t z) {
            const bool fires =
                coin == Coin::Legacy
                    ? stratum::surface::verticalGradientFires(
                          legacy, x, y, z, gradient.trueAtAndBelow, gradient.falseAtAndAbove)
                    : stratum::surface::verticalGradientFires(
                          source, x, y, z, gradient.trueAtAndBelow, gradient.falseAtAndAbove);
            return gradient.inverted ? !fires : fires;
        });
}

/// Scores the shipped executor: bedrock wherever its compiled tree places
/// bedrock. The tree carries its own polarity (the roof's `not`), so none is
/// applied here.
[[nodiscard]] BandScore scoreExecutor(const std::filesystem::path& region, const Gradient& gradient,
                                      const stratum::surface::Executor& executor) {
    return scoreBand(region, gradient,
                     [&](const std::int32_t x, const std::int32_t y, const std::int32_t z) {
                         stratum::surface::Context at;
                         at.x = x;
                         at.y = y;
                         at.z = z;
                         const stratum::settings::BlockState* placed = executor.apply(at);
                         return placed != nullptr &&
                                placed->name == ResourceLocation::parse("minecraft:bedrock");
                     });
}

/// The rules at the HEAD of @p surfaceRule's top-level sequence that place
/// bedrock directly, as a sequence of their own.
///
/// WHY THIS IS THE WHOLE TREE'S ANSWER, not a convenience. A sequence's first
/// rule to place a block wins, so rules that come first are pre-empted by
/// nothing: at every block the reduced tree places bedrock exactly where the
/// full one does, and places nothing where the full one places something
/// else. Only the head counts — a bedrock rule after any other rule could be
/// pre-empted by it — so the scan stops at the first rule of another shape.
/// And the reduced tree names no noise and reads no biome, which is what
/// lets it compile under a legacy source while the Nether's whole tree,
/// whose later rules read named noises, cannot (SPEC §11).
[[nodiscard]] nlohmann::json leadingBedrockRules(const nlohmann::json& surfaceRule) {
    REQUIRE(surfaceRule.value("type", "") == "minecraft:sequence");
    nlohmann::json head = nlohmann::json::array();
    for (const nlohmann::json& entry : surfaceRule.at("sequence")) {
        const bool placesBedrock =
            entry.value("type", "") == "minecraft:condition" && entry.contains("then_run") &&
            entry.at("then_run").value("type", "") == "minecraft:block" &&
            entry.at("then_run").contains("result_state") &&
            entry.at("then_run").at("result_state").value("Name", "") == "minecraft:bedrock";
        if (!placesBedrock) {
            break;
        }
        head.push_back(entry);
    }
    return nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", head}};
}

/// The agreement an independent predictor drawing from the same per-level
/// probability would reach, over the same band. This is the null the Nether
/// numbers are read against, and it is computed from the anchors rather than
/// written down.
[[nodiscard]] double chanceOver(const Gradient& gradient) {
    const auto span = static_cast<double>(gradient.falseAtAndAbove - gradient.trueAtAndBelow);
    double levels = 0.0;
    for (std::int32_t y = gradient.trueAtAndBelow + 1; y < gradient.falseAtAndAbove; ++y) {
        const double p = static_cast<double>(gradient.falseAtAndAbove - y) / span;
        levels += (p * p) + ((1.0 - p) * (1.0 - p));
    }
    return levels * static_cast<double>(kColumns);
}

} // namespace

TEST_CASE("a legacy dimension's vertical_gradient is not Xoroshiro, and the modern one is",
          "[conformance][legacy][surface][gradient]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    if (!std::filesystem::is_directory(tree)) {
        SKIP("no worldgen tree at " << tree << "; run tools/fetch-vanilla");
    }
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);

    const auto netherId = ResourceLocation::parse("minecraft:nether");
    const NoiseSettings& nether = loaded.settings.at(netherId);
    REQUIRE(nether.legacyRandomSource);
    const auto netherRules = stratum::surface::RuleGraph::resolve(nether.surfaceRule, netherId);
    const std::vector<Gradient> netherGradients = bedrockGradientsOf(netherRules, nether.geometry);

    const auto overworldId = ResourceLocation::parse("minecraft:overworld");
    const NoiseSettings& overworld = loaded.settings.at(overworldId);
    REQUIRE_FALSE(overworld.legacyRandomSource);
    const auto overworldRules =
        stratum::surface::RuleGraph::resolve(overworld.surfaceRule, overworldId);
    const std::vector<Gradient> overworldGradients =
        bedrockGradientsOf(overworldRules, overworld.geometry);

    // The premise, asserted so that a pack change cannot make the rest
    // vacuous: the Nether writes its bedrock with two gradients, floor and
    // roof, and exactly one of them is inverted.
    REQUIRE(netherGradients.size() == 2U);
    std::size_t invertedCount = 0;
    for (const Gradient& gradient : netherGradients) {
        invertedCount += static_cast<std::size_t>(gradient.inverted);
        // Four probabilistic levels each, which is what makes chance 39322.
        CHECK(gradient.falseAtAndAbove - gradient.trueAtAndBelow == 5);
    }
    CHECK(invertedCount == 1U);
    // The overworld has exactly one bedrock gradient — a floor, no roof —
    // and it is the control. Pinned so that the control cannot quietly
    // become an empty loop.
    REQUIRE(overworldGradients.size() == 1U);
    CHECK(overworldGradients.front().randomName == "minecraft:bedrock_floor");
    CHECK_FALSE(overworldGradients.front().inverted);

    SECTION("the legacy Nether") {
        // THE SHIPPED EXECUTOR, scored on the same bedrock: the Nether's two
        // bedrock rules, which head its tree and so decide bedrock-or-not at
        // every block exactly as the whole tree does (leadingBedrockRules),
        // compiled under a registry that declares each source. Pinned to the
        // two gradients scored below, so the reduction cannot quietly drop
        // one.
        const nlohmann::json bedrockJson = leadingBedrockRules(nether.surfaceRule);
        REQUIRE(bedrockJson.at("sequence").size() == 2U);
        const auto bedrockRules = stratum::surface::RuleGraph::resolve(bedrockJson, netherId);
        const std::vector<Gradient> reduced = bedrockGradientsOf(bedrockRules, nether.geometry);
        REQUIRE(reduced.size() == netherGradients.size());
        for (std::size_t i = 0; i < reduced.size(); ++i) {
            CHECK(reduced[i].randomName == netherGradients[i].randomName);
            CHECK(reduced[i].inverted == netherGradients[i].inverted);
        }
        const std::vector<ResourceLocation> nothing;

        std::size_t seedsMeasured = 0;
        for (const std::int64_t seed : kSeeds) {
            const std::filesystem::path region =
                fixtures() / "regions" / ("seed-" + std::to_string(seed)) / "nether" / "r.0.0.mca";
            if (!std::filesystem::is_regular_file(region)) {
                continue;
            }
            ++seedsMeasured;
            const auto legacyNoises = stratum::density::NoiseRegistry::create(
                pack, nothing, seed, stratum::density::RandomSource::Legacy);
            const auto modernNoises = stratum::density::NoiseRegistry::create(
                pack, nothing, seed, stratum::density::RandomSource::Xoroshiro);
            const auto legacyExecutor = stratum::surface::Executor::compile(
                bedrockRules, seed, nether.geometry, &legacyNoises, nether.seaLevel);
            const auto modernExecutor = stratum::surface::Executor::compile(
                bedrockRules, seed, nether.geometry, &modernNoises, nether.seaLevel);
            for (const Gradient& gradient : netherGradients) {
                const BandScore score = scoreGradient(region, seed, gradient);
                const double chance = chanceOver(gradient);
                CAPTURE(seed, gradient.randomName, gradient.inverted, score.scored, score.agree,
                        chance);
                REQUIRE(score.scored == kColumns * 4U);

                WARN("nether " << gradient.randomName << " seed " << seed << ": " << score.agree
                               << " / " << score.scored << " agree, chance is " << chance);

                // AT CHANCE, stated as a bound in both directions rather than
                // as a point: the sample's standard deviation under the null
                // is about 125 positions, so a window of 1000 is eight sigma
                // wide and a correct derivation — which would score 65536 —
                // misses it by more than 26000.
                CHECK(static_cast<double>(score.agree) > chance - 1000.0);
                CHECK(static_cast<double>(score.agree) < chance + 1000.0);
                // And it is emphatically not right, which is why the
                // executor does not run it under a legacy source (this file
                // once pinned a refusal on exactly this line).
                CHECK(score.agree < score.scored);

                // THE LEGACY PRIMITIVE, recovered on the aquifer's lattice
                // and never fitted to bedrock: exact, every position, where
                // the Xoroshiro coin above is at chance.
                const BandScore legacy = scoreGradient(region, seed, gradient, Coin::Legacy);
                WARN("nether " << gradient.randomName << " seed " << seed << ", legacy primitive: "
                               << legacy.agree << " / " << legacy.scored << " agree");
                CHECK(legacy.scored == score.scored);
                CHECK(legacy.agree == legacy.scored);

                // AND THE EXECUTOR THAT SHIPS, which runs that primitive
                // under a legacy registry: exact too. Under a modern registry
                // it is the Xoroshiro coin above, number for number — so
                // what decides the Nether's bedrock is the registry's
                // declared source and nothing else.
                const BandScore shipped = scoreExecutor(region, gradient, legacyExecutor);
                WARN("nether " << gradient.randomName << " seed " << seed << ", executor: "
                               << shipped.agree << " / " << shipped.scored << " agree");
                CHECK(shipped.scored == score.scored);
                CHECK(shipped.agree == shipped.scored);
                const BandScore shippedModern = scoreExecutor(region, gradient, modernExecutor);
                CHECK(shippedModern.agree == score.agree);

                // THE LADDER, asserted so the anchors are pinned. Vanilla's
                // own bedrock over the four levels follows 80/60/40/20% of
                // the columns (reversed where the gradient is inverted), and
                // if it did not, the band being scored would not be the band
                // the anchors name and every number above would be about
                // something else.
                std::size_t level = 0;
                for (const auto& [y, count] : score.bedrockByY) {
                    const auto span =
                        static_cast<double>(gradient.falseAtAndAbove - gradient.trueAtAndBelow);
                    const double p = static_cast<double>(gradient.falseAtAndAbove - y) / span;
                    const double expected =
                        (gradient.inverted ? 1.0 - p : p) * static_cast<double>(kColumns);
                    CAPTURE(y, count, expected);
                    // 16384 draws at p: a window of 400 is over six sigma at
                    // the widest level, so this pins the rung without being
                    // a re-measurement of the coin.
                    CHECK(static_cast<double>(count) > expected - 400.0);
                    CHECK(static_cast<double>(count) < expected + 400.0);
                    ++level;
                }
                CHECK(level == 4U);

                // The ends of the ladder: 100% at the certain anchor, 0% at
                // the impossible one. Only where the anchor is inside the
                // dimension — the Nether's roof `falseAtAndAbove` is 128,
                // one past the top, and there is no block there to read.
                if (score.certainColumns > 0U) {
                    CAPTURE(score.certainColumns, score.certainTrue, score.certainFalse);
                    CHECK(score.certainTrue == score.certainColumns);
                    CHECK(score.certainFalse == 0U);
                }
            }
        }
        if (seedsMeasured == 0) {
            SKIP("no golden Nether regions under "
                 << STRATUM_FIXTURES_DIR
                 << "; generate them with tools/fetch-vanilla --generate-regions --accept-eula");
        }
        CHECK(seedsMeasured == kSeeds.size());
    }

    SECTION("the modern overworld, same code, same seeds") {
        std::size_t seedsMeasured = 0;
        for (const std::int64_t seed : kSeeds) {
            const std::filesystem::path region = fixtures() / "regions" /
                                                 ("seed-" + std::to_string(seed)) / "overworld" /
                                                 "r.0.0.mca";
            if (!std::filesystem::is_regular_file(region)) {
                continue;
            }
            ++seedsMeasured;
            for (const Gradient& gradient : overworldGradients) {
                const BandScore score = scoreGradient(region, seed, gradient);
                CAPTURE(seed, gradient.randomName, gradient.inverted, score.scored);
                WARN("overworld " << gradient.randomName << " seed " << seed << ": " << score.agree
                                  << " / " << score.scored << " agree");
                // EXACT. Not "close": the control's whole job is to show that
                // the Nether's shortfall is the seeding and not this code.
                CHECK(score.agree == score.scored);
            }
        }
        if (seedsMeasured == 0) {
            SKIP("no golden overworld regions under "
                 << STRATUM_FIXTURES_DIR
                 << "; generate them with tools/fetch-vanilla --generate-regions --accept-eula");
        }
        CHECK(seedsMeasured == kSeeds.size());
    }
}

TEST_CASE("a legacy dimension's gradient compiles on both paths, and the End still does",
          "[conformance][legacy][surface][gradient]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    if (!std::filesystem::is_directory(tree)) {
        SKIP("no worldgen tree at " << tree << "; run tools/fetch-vanilla");
    }
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);

    const std::vector<ResourceLocation> nothing;
    const auto legacyRegistry = stratum::density::NoiseRegistry::create(
        pack, nothing, 0, stratum::density::RandomSource::Legacy);

    // NO LONGER REFUSED, on either path. This case once asserted the
    // opposite: a legacy dimension naming a gradient threw by name from
    // ChunkFiller::compile and Executor::compile, because the only coin this
    // build had was the Xoroshiro one, measured above to agree at chance.
    // The executor runs the measured legacy coin now. Pack-only, so CI runs
    // it on every push: the Nether's own bedrock rules — the head of its
    // tree, which names no noise — compile on the direct path and through
    // the filler, under the legacy registry, with nothing blocked.
    const auto netherId = ResourceLocation::parse("minecraft:nether");
    const NoiseSettings& nether = loaded.settings.at(netherId);
    REQUIRE(nether.legacyRandomSource);
    const auto bedrockRules =
        stratum::surface::RuleGraph::resolve(leadingBedrockRules(nether.surfaceRule), netherId);
    REQUIRE(stratum::surface::verticalGradientNames(bedrockRules) ==
            std::vector<std::string>{"minecraft:bedrock_floor", "minecraft:bedrock_roof"});
    CHECK_NOTHROW(stratum::surface::Executor::compile(bedrockRules, 0, nether.geometry,
                                                      &legacyRegistry, nether.seaLevel));
    const auto bedrockFiller =
        stratum::terrain::ChunkFiller::compile(loaded.graph, legacyRegistry, nether, &bedrockRules);
    CHECK(bedrockFiller.runsSurfaceRules());
    CHECK(bedrockFiller.surfaceRulesBlockedBy().empty());

    // The Nether's WHOLE tree is still blocked under the legacy source — but
    // by what it reads (named noises, the biome, a surface depth), which is
    // the unsolved named-noise seeding (SPEC §11), never by its gradients.
    const auto netherRules = stratum::surface::RuleGraph::resolve(nether.surfaceRule, netherId);
    const auto netherFiller =
        stratum::terrain::ChunkFiller::compile(loaded.graph, legacyRegistry, nether, &netherRules);
    CHECK_FALSE(netherFiller.runsSurfaceRules());
    CHECK_FALSE(netherFiller.surfaceRulesBlockedBy().empty());
    for (const std::string& reason : netherFiller.surfaceRulesBlockedBy()) {
        INFO(reason);
        CHECK(reason.find("vertical_gradient") == std::string::npos);
    }

    // THE END MUST STILL PASS, as it did when the gradient was refused. Its
    // tree has no gradient and both flags are false.
    const auto endId = ResourceLocation::parse("minecraft:end");
    const NoiseSettings& end = loaded.settings.at(endId);
    REQUIRE(end.legacyRandomSource);
    CHECK_FALSE(end.aquifersEnabled);
    CHECK_FALSE(end.oreVeinsEnabled);
    const auto endRules = stratum::surface::RuleGraph::resolve(end.surfaceRule, endId);
    CHECK(stratum::surface::verticalGradientNames(endRules).empty());
    const auto endFiller =
        stratum::terrain::ChunkFiller::compile(loaded.graph, legacyRegistry, end, &endRules);
    CHECK(endFiller.runsSurfaceRules());
    CHECK(endFiller.surfaceRulesBlockedBy().empty());
}

TEST_CASE("a doctored legacy pack with a gradient generates through CompiledDimension on the "
          "legacy coin, and the real End still compiles",
          "[conformance][legacy][surface][gradient][world]") {
    // THE PUBLIC PATH, which every native binding generates through. This
    // case once asserted a throw here: the gradient refusal was first a
    // non-fatal entry in ChunkFiller::surfaceRulesBlockedBy(), which nothing
    // on this path consults, so this pack compiled clean and filled 13549
    // blocks with its surface rules silently dropped — the best-effort
    // partial load SPEC §8 forbids — and the refusal was made to throw. The
    // legacy coin is measured now (the first case above scores it, and the
    // executor, on the golden Nether), so the same pack GENERATES, and what
    // this pins is that the public path reaches that coin: every block of
    // the band the gradient decides follows it, and the Xoroshiro coin would
    // have placed something else on a large share of them.
    //
    // The CONTROL runs first and in the same case on purpose, as it did
    // when this asserted a throw: the undoctored End, paired with any biome
    // list that exists, must compile through the identical path.
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path lists = fixtures() / "biome_parameters";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_directory(lists)) {
        SKIP("no worldgen or biome_parameters fixtures under " << fixtures()
                                                               << "; run tools/fetch-vanilla");
    }
    const auto endId = ResourceLocation::parse("minecraft:end");
    const auto overworldList = ResourceLocation::parse("minecraft:overworld");
    constexpr std::int64_t kSeed = 42;

    {
        const auto pack = stratum::data::Pack::open(tree);
        auto pipeline =
            stratum::freeze::read(stratum::freeze::write(stratum::freeze::resolve(pack, lists)));
        CHECK_NOTHROW(stratum::world::CompiledDimension::compile(std::move(pipeline), endId,
                                                                 overworldList, kSeed));
    }

    // A private copy of the pinned tree, with ONE edit: a gradient in front
    // of the End's end_stone rule, salted as vanilla's bedrock floor is but
    // anchored inside the main island — the End's own floor band is void,
    // where a surface rule places nothing. Removed on every exit, including
    // a failing assertion.
    struct TempTree {
        std::filesystem::path dir;

        ~TempTree() {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }
    };

    constexpr std::int32_t kTrueAtAndBelow = 50;
    constexpr std::int32_t kFalseAtAndAbove = 60;
    const std::string name = "minecraft:bedrock_floor";
    const TempTree tmp{stratum::test::tempPath("stratum-doctored-gradient")};
    std::filesystem::copy(tree, tmp.dir, std::filesystem::copy_options::recursive);

    const std::filesystem::path endJson = tmp.dir / "noise_settings" / "end.json";
    nlohmann::json end;
    {
        std::ifstream in(endJson);
        REQUIRE(in.good());
        in >> end;
    }
    REQUIRE(end.value("legacy_random_source", false));
    REQUIRE_FALSE(end.value("aquifers_enabled", true));
    REQUIRE_FALSE(end.value("ore_veins_enabled", true));
    const nlohmann::json original = end.at("surface_rule");
    end["surface_rule"] = {
        {"type", "minecraft:sequence"},
        {"sequence",
         {{{"type", "minecraft:condition"},
           {"if_true",
            {{"type", "minecraft:vertical_gradient"},
             {"random_name", name},
             {"true_at_and_below", {{"absolute", kTrueAtAndBelow}}},
             {"false_at_and_above", {{"absolute", kFalseAtAndAbove}}}}},
           {"then_run",
            {{"type", "minecraft:block"}, {"result_state", {{"Name", "minecraft:bedrock"}}}}}},
          original}}};
    {
        std::ofstream out(endJson);
        REQUIRE(out.good());
        out << end.dump(1);
    }

    const auto doctored = stratum::data::Pack::open(tmp.dir);
    auto pipeline =
        stratum::freeze::read(stratum::freeze::write(stratum::freeze::resolve(doctored, lists)));
    std::unique_ptr<stratum::world::CompiledDimension> dimension;
    REQUIRE_NOTHROW(dimension = stratum::world::CompiledDimension::compile(
                        std::move(pipeline), endId, overworldList, kSeed));
    REQUIRE(dimension != nullptr);

    const auto legacy = stratum::rng::legacyPositionalSourceFor(kSeed, name);
    const auto modern = stratum::rng::positionalSourceFor(kSeed, name);
    const auto air = ResourceLocation::parse("minecraft:air");
    const auto bedrock = ResourceLocation::parse("minecraft:bedrock");
    std::size_t solid = 0;
    std::size_t placed = 0;
    std::size_t mismatches = 0;
    std::size_t coinsDiffer = 0;
    std::size_t certainSolid = 0;
    std::size_t certainBedrock = 0;
    std::size_t impossibleBedrock = 0;
    // The four chunks around the island's centre.
    for (const std::int32_t cz : {-1, 0}) {
        for (const std::int32_t cx : {-1, 0}) {
            stratum::terrain::ChunkBuffer buffer(dimension->geometry());
            dimension->fillBlocks(cx, cz, buffer);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    // The surface pass leaves air alone, so only a solid
                    // block is the gradient's decision.
                    for (std::int32_t y = kTrueAtAndBelow + 1; y < kFalseAtAndAbove; ++y) {
                        const auto& block = buffer.at(lx, y, lz).name;
                        if (block == air) {
                            continue;
                        }
                        ++solid;
                        const bool fires = stratum::surface::verticalGradientFires(
                            legacy, x, y, z, kTrueAtAndBelow, kFalseAtAndAbove);
                        const bool isBedrock = block == bedrock;
                        placed += static_cast<std::size_t>(isBedrock);
                        mismatches += static_cast<std::size_t>(isBedrock != fires);
                        coinsDiffer += static_cast<std::size_t>(
                            fires != stratum::surface::verticalGradientFires(
                                         modern, x, y, z, kTrueAtAndBelow, kFalseAtAndAbove));
                    }
                    // And the two certain rungs: bedrock on every solid block
                    // at the lower anchor, on none at the upper.
                    const auto& atTrue = buffer.at(lx, kTrueAtAndBelow, lz).name;
                    if (atTrue != air) {
                        ++certainSolid;
                        certainBedrock += static_cast<std::size_t>(atTrue == bedrock);
                    }
                    impossibleBedrock += static_cast<std::size_t>(
                        buffer.at(lx, kFalseAtAndAbove, lz).name == bedrock);
                }
            }
        }
    }
    CAPTURE(solid, placed, mismatches, coinsDiffer, certainSolid, certainBedrock,
            impossibleBedrock);
    WARN("doctored End, seed " << kSeed << ": " << solid << " solid band blocks, " << placed
                               << " bedrock, " << mismatches << " off the legacy coin, "
                               << coinsDiffer << " where the Xoroshiro coin differs");
    // The band has to be mostly inside the island for this to say anything.
    CHECK(solid > 5000U);
    CHECK(mismatches == 0U);
    CHECK(coinsDiffer > solid / 4U);
    CHECK(certainSolid > 0U);
    CHECK(certainBedrock == certainSolid);
    CHECK(impossibleBedrock == 0U);
}
