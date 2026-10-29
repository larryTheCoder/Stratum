// Stratum — the surface executor's compiled rule list, over every condition.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// surface::Executor runs a tree as a straight list of tests and jumps, and
// each test op names its condition's type and runs that type's own test.
// These cases hold the list, over random trees of EVERY condition type and
// every rule type, to two things it must agree with block for block:
//
//   - a reference computed beside the executor, restating each condition's
//     documented semantics over the public reads (surfaceDepth, freezing,
//     bandlandsAt, a noise sampled at the column), and
//   - the executor's own recursive walk of the same tree, which a tree too
//     long to lower (past the list's bound) still takes.
//
// With and without a column memo, and with each block's biome handed over by
// name, by the executor's key, or both.
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using stratum::data::ResourceLocation;
using stratum::surface::Context;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

constexpr std::int64_t kSeed = 42;
constexpr std::int32_t kSeaLevel = 63;

[[nodiscard]] stratum::settings::NoiseGeometry geometry() {
    return stratum::settings::NoiseGeometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
}

/// The four noises every tree below may read, written on the fly.
class ProgramNoises {
public:
    /// @p surface is `minecraft:surface`'s own definition: vanilla's shape by
    /// default, or amplitudes no surface depth survives, to draw refusals.
    explicit ProgramNoises(
        std::string_view surface = R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})")
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-surface-program-test")) {
        const std::filesystem::path noiseDir = path_ / "noise";
        std::filesystem::create_directories(noiseDir);
        write(noiseDir, "surface", surface);
        write(noiseDir, "surface_secondary",
              R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 0.0, 1.0]})");
        write(noiseDir, "clay_bands_offset", R"({"firstOctave": -8, "amplitudes": [1.0]})");
        write(noiseDir, "bumps", R"({"firstOctave": -3, "amplitudes": [1.0, 1.0]})");
    }

    ProgramNoises(const ProgramNoises&) = delete;
    ProgramNoises& operator=(const ProgramNoises&) = delete;
    ProgramNoises(ProgramNoises&&) = delete;
    ProgramNoises& operator=(ProgramNoises&&) = delete;

    ~ProgramNoises() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::density::NoiseRegistry registry() const {
        return stratum::density::NoiseRegistry::create(stratum::data::Pack::openWorldgenTree(path_),
                                                       names_, kSeed,
                                                       stratum::density::RandomSource::Xoroshiro);
    }

private:
    void write(const std::filesystem::path& dir, const std::string& name, std::string_view json) {
        std::ofstream out(dir / (name + ".json"));
        out << json;
        names_.push_back(ResourceLocation::parse("minecraft:" + name));
    }

    std::filesystem::path path_;
    std::vector<ResourceLocation> names_;
};

/// A small deterministic generator for the random trees and blocks below.
class Dice {
public:
    explicit Dice(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
        state_ = (state_ * UINT64_C(6364136223846793005)) + UINT64_C(1442695040888963407);
        return static_cast<std::uint32_t>(state_ >> 33U) % bound;
    }

    /// Uniform in [low, low + span).
    [[nodiscard]] std::int32_t from(std::int32_t low, std::uint32_t span) {
        return low + static_cast<std::int32_t>(below(span));
    }

private:
    std::uint64_t state_;
};

constexpr std::array<std::string_view, 4> kBiomes{"minecraft:plains", "minecraft:desert",
                                                  "minecraft:badlands", "stratum:elsewhere"};

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

/// A condition a block's y alone can decide, anchored anywhere from below
/// the dimension's floor to above its top: a gradient, or a y_above with no
/// multiplier and no stone depth (Executor's y bands).
[[nodiscard]] nlohmann::json yDecided(Dice& dice) {
    const std::int32_t low = dice.from(-100, 460);
    if (dice.below(2) == 0U) {
        return nlohmann::json{{"type", "minecraft:y_above"},
                              {"anchor", {{"absolute", low}}},
                              {"surface_depth_multiplier", 0},
                              {"add_stone_depth", false}};
    }
    return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                          {"random_name", dice.below(2) == 0U ? "stratum:a" : "stratum:b"},
                          {"true_at_and_below", {{"absolute", low}}},
                          {"false_at_and_above", {{"absolute", low + dice.from(-3, 40)}}}};
}

[[nodiscard]] nlohmann::json randomCondition(Dice& dice, int depth, bool wide = false) {
    if (wide && dice.below(2) == 0U) {
        return yDecided(dice);
    }
    switch (dice.below(depth < 2 ? 12U : 11U)) {
        case 0:
            return nlohmann::json{{"type", "minecraft:y_above"},
                                  {"anchor", {{"absolute", dice.from(40, 60)}}},
                                  {"surface_depth_multiplier", dice.from(-2, 5)},
                                  {"add_stone_depth", dice.below(2) == 0U}};
        case 1:
            return nlohmann::json{{"type", "minecraft:water"},
                                  {"offset", dice.from(-6, 8)},
                                  {"surface_depth_multiplier", dice.from(-1, 3)},
                                  {"add_stone_depth", dice.below(2) == 0U}};
        case 2:
            return nlohmann::json{{"type", "minecraft:stone_depth"},
                                  {"offset", dice.from(0, 4)},
                                  {"surface_type", dice.below(2) == 0U ? "floor" : "ceiling"},
                                  {"add_surface_depth", dice.below(2) == 0U},
                                  {"secondary_depth_range", dice.below(3) == 0U ? 6 : 0}};
        case 3:
            return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                                  {"random_name", dice.below(2) == 0U ? "stratum:a" : "stratum:b"},
                                  {"true_at_and_below", {{"absolute", dice.from(40, 20)}}},
                                  {"false_at_and_above", {{"absolute", dice.from(64, 30)}}}};
        case 4:
            return nlohmann::json{{"type", "minecraft:hole"}};
        case 5:
            return nlohmann::json{{"type", "minecraft:steep"}};
        case 6: {
            const double low = (static_cast<double>(dice.below(16)) - 8.0) / 10.0;
            return nlohmann::json{
                {"type", "minecraft:noise_threshold"},
                {"noise", dice.below(2) == 0U ? "minecraft:bumps" : "minecraft:surface"},
                {"min_threshold", low},
                {"max_threshold", low + 0.6}};
        }
        case 7:
            return nlohmann::json{{"type", "minecraft:above_preliminary_surface"}};
        case 8: {
            nlohmann::json biomes = nlohmann::json::array();
            const std::uint32_t count = 1U + dice.below(2);
            for (std::uint32_t i = 0; i < count; ++i) {
                biomes.push_back(std::string(kBiomes[dice.below(3)]));
            }
            return nlohmann::json{{"type", "minecraft:biome"}, {"biome_is", biomes}};
        }
        case 9:
            return nlohmann::json{{"type", "minecraft:temperature"}};
        case 10:
            return nlohmann::json{{"type", "minecraft:y_above"},
                                  {"anchor", {{"absolute", dice.from(40, 60)}}},
                                  {"surface_depth_multiplier", 0},
                                  {"add_stone_depth", false}};
        default:
            return nlohmann::json{{"type", "minecraft:not"},
                                  {"invert", randomCondition(dice, depth + 1, wide)}};
    }
}

[[nodiscard]] nlohmann::json randomRule(Dice& dice, int depth, bool wide = false) {
    const std::uint32_t kind = depth >= 5 ? dice.below(6) / 5U : dice.below(5);
    if (kind == 0U) {
        return block("stratum:b" + std::to_string(dice.below(6)));
    }
    if (kind == 1U) {
        return nlohmann::json{{"type", "minecraft:bandlands"}};
    }
    if (kind == 2U) {
        nlohmann::json children = nlohmann::json::array();
        const std::uint32_t count = 1U + dice.below(4);
        for (std::uint32_t i = 0; i < count; ++i) {
            children.push_back(randomRule(dice, depth + 1, wide));
        }
        return nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", children}};
    }
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", randomCondition(dice, 0, wide)},
                          {"then_run", randomRule(dice, depth + 1, wide)}};
}

/// The documented semantics, walked by hand over the resolved graph, every
/// noise-derived read taken from the executor's PUBLIC, uncached functions or
/// the registry directly: the reference the compiled list is held to.
class Reference {
public:
    Reference(const RuleGraph& graph, const Executor& executor,
              const stratum::density::NoiseRegistry& noises)
        : graph_(&graph), executor_(&executor), noises_(&noises),
          a_(stratum::rng::positionalSourceFor(kSeed, "stratum:a")),
          b_(stratum::rng::positionalSourceFor(kSeed, "stratum:b")) {}

    [[nodiscard]] const stratum::settings::BlockState* rule(stratum::surface::RuleIndex index,
                                                            const Context& at) const {
        const stratum::surface::Rule& rule = graph_->rule(index);
        switch (rule.type) {
            case stratum::surface::RuleType::Block:
                return &rule.block;
            case stratum::surface::RuleType::Bandlands:
                return &executor_->bandlandsAt(at.x, at.y, at.z);
            case stratum::surface::RuleType::Sequence:
                for (const stratum::surface::RuleIndex child : rule.sequence) {
                    if (const auto* placed = this->rule(child, at); placed != nullptr) {
                        return placed;
                    }
                }
                return nullptr;
            case stratum::surface::RuleType::Condition:
                return test(rule.condition, at) ? this->rule(rule.thenRun, at) : nullptr;
            default:
                throw std::logic_error("not generated");
        }
    }

private:
    [[nodiscard]] double column(std::string_view noise, const Context& at) const {
        return noises_->get(ResourceLocation::parse(std::string(noise)))
            .sample(static_cast<double>(at.x), 0.0, static_cast<double>(at.z));
    }

    [[nodiscard]] std::int32_t depth(const Context& at) const {
        return executor_->surfaceDepth(at.x, at.z);
    }

    [[nodiscard]] bool test(stratum::surface::ConditionIndex index, const Context& at) const {
        using stratum::surface::ConditionType;
        const stratum::surface::Condition& condition = graph_->condition(index);
        const auto anchor = [&](const stratum::surface::VerticalAnchor& value) {
            return value.resolve(geometry_);
        };
        switch (condition.type) {
            case ConditionType::Not:
                return !test(condition.invert, at);
            case ConditionType::YAbove:
                return at.y + (condition.addStoneDepth ? at.stoneDepthAbove : 0) >=
                       anchor(condition.anchor) +
                           (condition.surfaceDepthMultiplier == 0
                                ? 0
                                : condition.surfaceDepthMultiplier * depth(at));
            case ConditionType::Water:
                return !at.waterHeight.has_value() ||
                       at.y + (condition.addStoneDepth ? at.stoneDepthAbove : 0) >=
                           *at.waterHeight + condition.offset +
                               (condition.surfaceDepthMultiplier == 0
                                    ? 0
                                    : condition.surfaceDepthMultiplier * depth(at));
            case ConditionType::StoneDepth: {
                const std::int32_t run =
                    condition.surfaceType == "ceiling" ? at.stoneDepthBelow : at.stoneDepthAbove;
                if (run <= 0) {
                    return false;
                }
                std::int32_t threshold = condition.offset;
                if (condition.addSurfaceDepth) {
                    threshold += depth(at);
                }
                if (condition.secondaryDepthRange != 0) {
                    threshold += static_cast<std::int32_t>(
                        (column("minecraft:surface_secondary", at) + 1.0) * 0.5 *
                        static_cast<double>(condition.secondaryDepthRange));
                }
                return run - 1 <= threshold;
            }
            case ConditionType::VerticalGradient:
                return stratum::surface::verticalGradientFires(
                    condition.randomName == "stratum:a" ? a_ : b_, at.x, at.y, at.z,
                    anchor(condition.trueAtAndBelow), anchor(condition.falseAtAndAbove));
            case ConditionType::Hole:
                return depth(at) <= 0;
            case ConditionType::Steep:
                return (at.heightWest - at.heightEast >= 4) ||
                       (at.heightSouth - at.heightNorth >= 4);
            case ConditionType::NoiseThreshold: {
                const double value = column(condition.noise->toString(), at);
                return condition.minThreshold <= value && value <= condition.maxThreshold;
            }
            case ConditionType::AbovePreliminarySurface:
                return at.y >= at.preliminarySurface + depth(at) - 8;
            case ConditionType::Biome:
                return std::ranges::find(condition.biomes, *at.biome) != condition.biomes.end();
            case ConditionType::Temperature:
                return executor_->freezing(at.x, at.y, at.z, at.biomeTemperature);
            default:
                throw std::logic_error("not generated");
        }
    }

    const RuleGraph* graph_;
    const Executor* executor_;
    const stratum::density::NoiseRegistry* noises_;
    stratum::settings::NoiseGeometry geometry_ = geometry();
    stratum::rng::PositionalSource a_;
    stratum::rng::PositionalSource b_;
};

/// A column's blocks, top down, each with every fact the rules read varied:
/// the column's own facts fixed, the per-block ones drawn.
template<typename Visit>
void walkColumns(Dice& dice, std::size_t columns, Visit&& visit) {
    for (std::size_t c = 0; c < columns; ++c) {
        Context here;
        here.x = dice.from(-40, 81);
        here.z = dice.from(-40, 81);
        here.preliminarySurface = dice.from(50, 40);
        if (dice.below(3) != 0U) {
            here.waterHeight = dice.from(55, 20);
        }
        here.heightWest = dice.from(60, 10);
        here.heightEast = dice.from(60, 10);
        here.heightNorth = dice.from(60, 10);
        here.heightSouth = dice.from(60, 10);
        const std::int32_t top = dice.from(70, 60);
        for (std::int32_t y = top; y > top - 40; --y) {
            here.y = y;
            here.stoneDepthAbove = static_cast<std::int32_t>(dice.below(12));
            here.stoneDepthBelow = static_cast<std::int32_t>(dice.below(12));
            if (y % 4 == 0 || here.y == top) {
                const std::uint32_t biome = dice.below(static_cast<std::uint32_t>(kBiomes.size()));
                here.biome = ResourceLocation::parse(std::string(kBiomes[biome]));
                here.biomeTemperature = dice.below(2) == 0U ? 0.1F : 0.8F;
            }
            visit(here);
        }
    }
}

[[nodiscard]] std::string nameOf(const stratum::settings::BlockState* placed) {
    return placed == nullptr ? std::string("nothing") : placed->name.toString();
}

} // namespace

TEST_CASE("the compiled rule list runs every condition type as its documented semantics",
          "[surface][program]") {
    const ProgramNoises tree;
    const auto noises = tree.registry();
    std::size_t placed = 0;
    std::size_t empty = 0;
    std::array<std::size_t, 3> modes{};
    for (std::uint64_t treeSeed = 1; treeSeed <= 150; ++treeSeed) {
        Dice dice(treeSeed);
        const RuleGraph graph =
            RuleGraph::resolve(randomRule(dice, 0), ResourceLocation::parse("stratum:test"));
        const Executor executor = Executor::compile(graph, kSeed, geometry(), &noises, kSeaLevel);
        const Reference reference(graph, executor, noises);
        stratum::surface::ColumnMemo memo;
        walkColumns(dice, 30, [&](const Context& block) {
            const auto* expected = reference.rule(graph.root(), block);
            CAPTURE(treeSeed, block.x, block.y, block.z);
            REQUIRE(executor.apply(block) == expected);
            // The biome by name, by key alone, or both.
            Context handed = block;
            const std::uint32_t mode = dice.below(3);
            if (mode != 0U) {
                handed.biomeKey = executor.biomeKey(*block.biome);
            }
            if (mode == 1U) {
                handed.biome.reset();
            }
            REQUIRE(executor.apply(handed, memo) == expected);
            ++modes.at(mode);
            (expected == nullptr ? empty : placed) += 1;
        });
    }
    CHECK(placed > 50000U);
    CHECK(empty > 50000U);
    CHECK(std::ranges::all_of(modes, [](std::size_t count) { return count > 30000U; }));
}

TEST_CASE("the compiled rule list places what the executor's own walk of the tree places",
          "[surface][program]") {
    // A tree whose list would pass kMaxProgram is walked instead (runRule and
    // test()), so prefixing each random tree with a never-true condition over
    // 66 000 blocks turns the list off for it — and nothing else: the
    // condition is false everywhere and the walk never enters its blocks.
    const ProgramNoises tree;
    const auto noises = tree.registry();
    nlohmann::json padding = nlohmann::json::array();
    for (int i = 0; i < 66000; ++i) {
        padding.push_back(block("minecraft:stone"));
    }
    const nlohmann::json never{
        {"type", "minecraft:condition"},
        {"if_true",
         {{"type", "minecraft:y_above"},
          {"anchor", {{"absolute", 10000}}},
          {"surface_depth_multiplier", 0},
          {"add_stone_depth", false}}},
        {"then_run", {{"type", "minecraft:sequence"}, {"sequence", padding}}}};
    std::size_t compared = 0;
    for (std::uint64_t treeSeed = 1001; treeSeed <= 1012; ++treeSeed) {
        Dice dice(treeSeed);
        const nlohmann::json json = randomRule(dice, 0);
        const RuleGraph listed = RuleGraph::resolve(json, ResourceLocation::parse("stratum:test"));
        const RuleGraph walked = RuleGraph::resolve(
            nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", {never, json}}},
            ResourceLocation::parse("stratum:test"));
        const Executor list = Executor::compile(listed, kSeed, geometry(), &noises, kSeaLevel);
        const Executor walk = Executor::compile(walked, kSeed, geometry(), &noises, kSeaLevel);
        stratum::surface::ColumnMemo listMemo;
        stratum::surface::ColumnMemo walkMemo;
        walkColumns(dice, 10, [&](const Context& block) {
            CAPTURE(treeSeed, block.x, block.y, block.z);
            REQUIRE(nameOf(list.apply(block, listMemo)) == nameOf(walk.apply(block, walkMemo)));
            ++compared;
        });
    }
    CHECK(compared == 12U * 10U * 40U);
}

TEST_CASE("each y band's copy of the list places what the list places, at every y and beyond",
          "[surface][program]") {
    // Executor cuts the dimension's height at every anchor of a test a y
    // alone decides and runs each band's own copy of the list, those tests
    // already answered. Random trees half made of such tests, anchored from
    // below the floor to above the top, are swept over every y of the
    // dimension and 30 beyond each end (where the list as lowered runs), and
    // held to the reference block for block — with the memo, and without.
    const ProgramNoises tree;
    const auto noises = tree.registry();
    std::size_t compared = 0;
    std::size_t placed = 0;
    for (std::uint64_t treeSeed = 2001; treeSeed <= 2060; ++treeSeed) {
        Dice dice(treeSeed);
        const RuleGraph graph =
            RuleGraph::resolve(randomRule(dice, 0, true), ResourceLocation::parse("stratum:test"));
        const Executor executor = Executor::compile(graph, kSeed, geometry(), &noises, kSeaLevel);
        const Reference reference(graph, executor, noises);
        stratum::surface::ColumnMemo memo;
        for (int column = 0; column < 4; ++column) {
            Context block;
            block.x = dice.from(-40, 81);
            block.z = dice.from(-40, 81);
            block.preliminarySurface = dice.from(50, 40);
            if (dice.below(2) == 0U) {
                block.waterHeight = dice.from(55, 20);
            }
            block.heightWest = dice.from(60, 10);
            block.heightSouth = dice.from(60, 10);
            block.biome = ResourceLocation::parse(std::string(kBiomes[dice.below(4)]));
            for (std::int32_t y = 320 + 30; y >= -64 - 30; --y) {
                block.y = y;
                block.stoneDepthAbove = static_cast<std::int32_t>(dice.below(8));
                block.stoneDepthBelow = static_cast<std::int32_t>(dice.below(8));
                const auto* expected = reference.rule(graph.root(), block);
                CAPTURE(treeSeed, block.x, y, block.z);
                REQUIRE(executor.apply(block, memo) == expected);
                REQUIRE(executor.apply(block) == expected);
                // And with the biome as this executor's key — its band's
                // keyed copy — alone and beside the name.
                Context keyed = block;
                keyed.biomeKey = executor.biomeKey(*block.biome);
                REQUIRE(executor.apply(keyed, memo) == expected);
                keyed.biome.reset();
                REQUIRE(executor.apply(keyed) == expected);
                placed += expected != nullptr ? 1U : 0U;
                ++compared;
            }
        }
    }
    CHECK(compared == 60U * 4U * 445U);
    CHECK(placed > compared / 4U);
}

TEST_CASE("a tree cut into more bands than the executor keeps runs its one list",
          "[surface][program]") {
    // 90 y_above conditions with distinct anchors in the dimension: 91 bands,
    // past the executor's bound, so the list as lowered serves every y.
    nlohmann::json sequence = nlohmann::json::array();
    for (int i = 0; i < 90; ++i) {
        const nlohmann::json above{{"type", "minecraft:y_above"},
                                   {"anchor", {{"absolute", -60 + (4 * i)}}},
                                   {"surface_depth_multiplier", 0},
                                   {"add_stone_depth", false}};
        const nlohmann::json below{{"type", "minecraft:not"}, {"invert", above}};
        sequence.push_back(nlohmann::json{{"type", "minecraft:condition"},
                                          {"if_true", below},
                                          {"then_run", block("stratum:b" + std::to_string(i))}});
    }
    const RuleGraph graph =
        RuleGraph::resolve(nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", sequence}},
                           ResourceLocation::parse("stratum:test"));
    const Executor executor = Executor::compile(graph, kSeed, geometry());
    for (std::int32_t y = -80; y < 340; ++y) {
        Context block;
        block.y = y;
        const auto* placed = executor.apply(block);
        CAPTURE(y);
        // The first anchor above y names the block: not (y >= -60 + 4i).
        const std::int32_t first = y < -60 ? 0 : ((y + 60) / 4) + 1;
        if (first >= 90) {
            CHECK(placed == nullptr);
        } else {
            REQUIRE(placed != nullptr);
            CHECK(placed->name.toString() == "stratum:b" + std::to_string(first));
        }
    }
}

namespace {

/// One of a small fixed set of conditions, so that a tree drawn from it
/// tests the same condition — the same type, the same parameters, in
/// another of its own entries — again and again, as vanilla's trees do.
[[nodiscard]] nlohmann::json pooledCondition(Dice& dice) {
    const auto stoneDepth = [](int offset, const char* type, bool addSurfaceDepth, int range) {
        return nlohmann::json{{"type", "minecraft:stone_depth"},
                              {"offset", offset},
                              {"surface_type", type},
                              {"add_surface_depth", addSurfaceDepth},
                              {"secondary_depth_range", range}};
    };
    const auto water = [](int offset, int multiplier, bool addStoneDepth) {
        return nlohmann::json{{"type", "minecraft:water"},
                              {"offset", offset},
                              {"surface_depth_multiplier", multiplier},
                              {"add_stone_depth", addStoneDepth}};
    };
    switch (dice.below(15)) {
        case 0:
            return stoneDepth(0, "floor", false, 0);
        case 1:
            return stoneDepth(0, "floor", true, 0);
        case 2:
            return stoneDepth(1, "ceiling", false, 6);
        case 3:
            return water(-1, 0, false);
        case 4:
            return water(-6, -1, true);
        case 5:
            return nlohmann::json{{"type", "minecraft:y_above"},
                                  {"anchor", {{"absolute", 63}}},
                                  {"surface_depth_multiplier", 1},
                                  {"add_stone_depth", true}};
        case 6:
            return nlohmann::json{{"type", "minecraft:biome"},
                                  {"biome_is", {"minecraft:plains", "minecraft:desert"}}};
        case 7:
            return nlohmann::json{{"type", "minecraft:biome"},
                                  {"biome_is", {"minecraft:desert", "minecraft:plains"}}};
        case 8:
            return nlohmann::json{{"type", "minecraft:hole"}};
        case 9:
            return nlohmann::json{{"type", "minecraft:above_preliminary_surface"}};
        case 10:
            return nlohmann::json{{"type", "minecraft:noise_threshold"},
                                  {"noise", "minecraft:bumps"},
                                  {"min_threshold", -0.2},
                                  {"max_threshold", 0.4}};
        case 11:
            return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                                  {"random_name", "stratum:a"},
                                  {"true_at_and_below", {{"absolute", 60}}},
                                  {"false_at_and_above", {{"absolute", 70}}}};
        case 12:
            return nlohmann::json{{"type", "minecraft:steep"}};
        case 13:
            return nlohmann::json{{"type", "minecraft:temperature"}};
        default:
            return nlohmann::json{{"type", "minecraft:not"}, {"invert", pooledCondition(dice)}};
    }
}

[[nodiscard]] nlohmann::json pooledRule(Dice& dice, int depth) {
    const std::uint32_t kind = depth >= 6 ? 0U : dice.below(5);
    if (kind == 0U) {
        return block("stratum:b" + std::to_string(dice.below(6)));
    }
    if (kind == 1U) {
        nlohmann::json children = nlohmann::json::array();
        const std::uint32_t count = 1U + dice.below(5);
        for (std::uint32_t i = 0; i < count; ++i) {
            children.push_back(pooledRule(dice, depth + 1));
        }
        return nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", children}};
    }
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", pooledCondition(dice)},
                          {"then_run", pooledRule(dice, depth + 1)}};
}

/// What one apply comes to: the block's name, "nothing", or the refusal's
/// own words.
template<typename Apply>
[[nodiscard]] std::string outcomeOf(Apply&& apply) {
    try {
        return nameOf(apply());
    } catch (const stratum::surface::ExecutionError& error) {
        return std::string("refused: ") + error.what();
    }
}

} // namespace

TEST_CASE("a test whose twin has answered is followed, not made again, refusals included",
          "[surface][program]") {
    // Trees drawn from a pool of fifteen conditions test each one many times
    // over; the executor answers each repeat from the first (the y bands'
    // copies, unrolled over what their walk has learnt). Under vanilla's
    // surface noise and under one whose amplitudes no surface depth
    // survives — so that the first of a twin to read the depth refuses —
    // every block comes to what the reference comes to, refusal for refusal,
    // with the memo and without, by name and by key.
    std::size_t refused = 0;
    std::size_t placed = 0;
    for (const std::string_view surface :
         {std::string_view{R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})"},
          std::string_view{R"({"firstOctave": -6, "amplitudes": [1e12]})"}}) {
        const ProgramNoises tree(surface);
        const auto noises = tree.registry();
        for (std::uint64_t treeSeed = 3001; treeSeed <= 3060; ++treeSeed) {
            Dice dice(treeSeed);
            const RuleGraph graph =
                RuleGraph::resolve(pooledRule(dice, 0), ResourceLocation::parse("stratum:test"));
            const Executor executor =
                Executor::compile(graph, kSeed, geometry(), &noises, kSeaLevel);
            const Reference reference(graph, executor, noises);
            stratum::surface::ColumnMemo memo;
            walkColumns(dice, 6, [&](const Context& block) {
                const std::string expected =
                    outcomeOf([&] { return reference.rule(graph.root(), block); });
                CAPTURE(treeSeed, block.x, block.y, block.z);
                REQUIRE(outcomeOf([&] { return executor.apply(block, memo); }) == expected);
                REQUIRE(outcomeOf([&] { return executor.apply(block); }) == expected);
                Context keyed = block;
                keyed.biomeKey = executor.biomeKey(*block.biome);
                keyed.biome.reset();
                REQUIRE(outcomeOf([&] { return executor.apply(keyed, memo); }) == expected);
                refused += expected.starts_with("refused: ") ? 1U : 0U;
                placed += expected.starts_with("stratum:") ? 1U : 0U;
            });
        }
    }
    CHECK(refused > 1000U);
    CHECK(placed > 10000U);
}
