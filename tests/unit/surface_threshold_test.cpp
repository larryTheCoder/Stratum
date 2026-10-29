// Stratum — above_preliminary_surface's threshold, kept per column and read ahead.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The condition is `y >= preliminarySurface + surfaceDepth - 8`, and the
// right-hand side is the column's: surface::ColumnMemo keeps it, with the
// preliminary surface it was computed from. Executor::decidedByThreshold
// then answers a block whose walk starts with that test and ends right
// after it, without running the rules — what terrain::ChunkFiller's surface
// pass asks before every block. These cases hold both to apply() without a
// memo, block for block and refusal for refusal:
//
//   * on vanilla's shape (a bedrock gradient, the condition over a tree,
//     a deepslate gradient) and on random trees that start with it or not;
//   * with the preliminary surface changing within a column, which must
//     recompute the threshold rather than read another one's;
//   * under a surface noise no depth survives and a preliminary surface no
//     sum survives, where the first block to read the threshold refuses and
//     every later one must too;
//   * and the shortcut declines for a memo of another column or another
//     executor, a fresh memo, or a walk it does not decide.
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using stratum::data::ResourceLocation;
using stratum::surface::ColumnMemo;
using stratum::surface::Context;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

constexpr std::int64_t kSeed = 20261030;
constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kTopY = 320;

[[nodiscard]] stratum::settings::NoiseGeometry geometry() {
    return stratum::settings::NoiseGeometry{
        .minY = kMinY, .height = kTopY - kMinY, .sizeHorizontal = 1, .sizeVertical = 2};
}

/// `minecraft:surface` and `minecraft:surface_secondary`, written on the fly;
/// @p surface is the first's definition.
class ThresholdNoises {
public:
    explicit ThresholdNoises(std::string_view surface)
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-surface-threshold-test")) {
        const std::filesystem::path noiseDir = path_ / "noise";
        std::filesystem::create_directories(noiseDir);
        write(noiseDir, "surface", surface);
        write(noiseDir, "surface_secondary",
              R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 0.0, 1.0]})");
    }

    ThresholdNoises(const ThresholdNoises&) = delete;
    ThresholdNoises& operator=(const ThresholdNoises&) = delete;
    ThresholdNoises(ThresholdNoises&&) = delete;
    ThresholdNoises& operator=(ThresholdNoises&&) = delete;

    ~ThresholdNoises() {
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

class Dice {
public:
    explicit Dice(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
        state_ = (state_ * UINT64_C(6364136223846793005)) + UINT64_C(1442695040888963407);
        return static_cast<std::uint32_t>(state_ >> 33U) % bound;
    }

    [[nodiscard]] std::int32_t from(std::int32_t low, std::uint32_t span) {
        return low + static_cast<std::int32_t>(below(span));
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

[[nodiscard]] nlohmann::json abovePreliminary() {
    return nlohmann::json{{"type", "minecraft:above_preliminary_surface"}};
}

[[nodiscard]] nlohmann::json gradient(const char* name, std::int32_t trueAt, std::int32_t falseAt) {
    return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                          {"random_name", name},
                          {"true_at_and_below", {{"absolute", trueAt}}},
                          {"false_at_and_above", {{"absolute", falseAt}}}};
}

[[nodiscard]] nlohmann::json condition(nlohmann::json test, nlohmann::json then) {
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", std::move(test)},
                          {"then_run", std::move(then)}};
}

/// A condition from a small pool, the threshold's own among them.
[[nodiscard]] nlohmann::json pooledCondition(Dice& dice) {
    switch (dice.below(7)) {
        case 0:
            return nlohmann::json{{"type", "minecraft:stone_depth"},
                                  {"offset", dice.from(0, 3)},
                                  {"surface_type", dice.below(2) == 0U ? "floor" : "ceiling"},
                                  {"add_surface_depth", dice.below(2) == 0U},
                                  {"secondary_depth_range", dice.below(2) == 0U ? 6 : 0}};
        case 1:
            return nlohmann::json{{"type", "minecraft:water"},
                                  {"offset", dice.from(-6, 8)},
                                  {"surface_depth_multiplier", dice.from(-1, 3)},
                                  {"add_stone_depth", dice.below(2) == 0U}};
        case 2:
            return nlohmann::json{{"type", "minecraft:y_above"},
                                  {"anchor", {{"absolute", dice.from(-40, 120)}}},
                                  {"surface_depth_multiplier", dice.from(-1, 3)},
                                  {"add_stone_depth", dice.below(2) == 0U}};
        case 3:
            return gradient("stratum:a", dice.from(-20, 60), dice.from(40, 60));
        case 4:
            return nlohmann::json{{"type", "minecraft:hole"}};
        case 5:
            return nlohmann::json{{"type", "minecraft:not"}, {"invert", abovePreliminary()}};
        default:
            return abovePreliminary();
    }
}

[[nodiscard]] nlohmann::json pooledRule(Dice& dice, int depth) {
    const std::uint32_t kind = depth >= 4 ? 0U : dice.below(4);
    if (kind == 0U) {
        return block("stratum:b" + std::to_string(dice.below(5)));
    }
    if (kind == 1U) {
        nlohmann::json children = nlohmann::json::array();
        const std::uint32_t count = 1U + dice.below(4);
        for (std::uint32_t i = 0; i < count; ++i) {
            children.push_back(pooledRule(dice, depth + 1));
        }
        return nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", children}};
    }
    return condition(pooledCondition(dice), pooledRule(dice, depth + 1));
}

/// Vanilla's overworld shape around @p surface: bedrock at the floor, the
/// threshold's condition over @p surface, deepslate below y 8.
[[nodiscard]] nlohmann::json vanillaShape(nlohmann::json surface) {
    return nlohmann::json{
        {"type", "minecraft:sequence"},
        {"sequence",
         {condition(gradient("minecraft:bedrock_floor", kMinY, kMinY + 5),
                    block("minecraft:bedrock")),
          condition(abovePreliminary(), std::move(surface)),
          condition(gradient("minecraft:deepslate", 0, 8), block("minecraft:deepslate"))}}};
}

[[nodiscard]] std::string nameOf(const stratum::settings::BlockState* placed) {
    return placed == nullptr ? std::string("nothing") : placed->name.toString();
}

template<typename Apply>
[[nodiscard]] std::string outcomeOf(Apply&& apply) {
    try {
        return nameOf(apply());
    } catch (const stratum::surface::ExecutionError& error) {
        return std::string("refused: ") + error.what();
    }
}

struct Tally {
    std::size_t blocks = 0;
    std::size_t decided = 0;
    std::size_t refused = 0;
};

/// Every y of a few columns, top down, as the surface pass walks them: the
/// shortcut first, apply where it declines, one memo for all of it — held
/// to apply() without a memo. The preliminary surface moves within a
/// column now and then.
void walk(const Executor& executor, Dice& dice, std::size_t columns, std::int32_t preliminaryLow,
          std::uint32_t preliminarySpan, Tally& tally) {
    ColumnMemo memo;
    for (std::size_t c = 0; c < columns; ++c) {
        Context at;
        at.x = dice.from(-200, 401);
        at.z = dice.from(-200, 401);
        at.preliminarySurface = dice.from(preliminaryLow, preliminarySpan);
        if (dice.below(2) == 0U) {
            at.waterHeight = dice.from(50, 30);
        }
        for (std::int32_t y = kTopY - 1; y >= kMinY; --y) {
            at.y = y;
            at.stoneDepthAbove = static_cast<std::int32_t>(dice.below(10));
            at.stoneDepthBelow = static_cast<std::int32_t>(dice.below(10));
            if (dice.below(97) == 0U) {
                at.preliminarySurface = dice.from(preliminaryLow, preliminarySpan);
            }
            const std::string expected = outcomeOf([&] { return executor.apply(at); });
            CAPTURE(at.x, at.y, at.z, at.preliminarySurface);
            const std::optional<const stratum::settings::BlockState*> decided =
                executor.decidedByThreshold(at, memo);
            if (decided.has_value()) {
                ++tally.decided;
                REQUIRE(nameOf(*decided) == expected);
            } else {
                REQUIRE(outcomeOf([&] { return executor.apply(at, memo); }) == expected);
            }
            tally.refused += expected.starts_with("refused: ") ? 1U : 0U;
            ++tally.blocks;
        }
    }
}

} // namespace

TEST_CASE("a column's kept threshold decides what apply decides, block for block",
          "[surface][program]") {
    const ThresholdNoises noises(R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})");
    const auto registry = noises.registry();
    Tally vanilla;
    Tally random;
    for (std::uint64_t treeSeed = 1; treeSeed <= 40; ++treeSeed) {
        Dice dice(treeSeed);
        CAPTURE(treeSeed);
        const RuleGraph shaped =
            RuleGraph::resolve(vanillaShape(pooledRule(dice, 0)), ResourceLocation{"stratum", "t"});
        const Executor onShape = Executor::compile(shaped, kSeed, geometry(), &registry, kSeaLevel);
        walk(onShape, dice, 3, 20, 80, vanilla);
        const RuleGraph drawn =
            RuleGraph::resolve(pooledRule(dice, 0), ResourceLocation{"stratum", "t"});
        const Executor onDrawn = Executor::compile(drawn, kSeed, geometry(), &registry, kSeaLevel);
        walk(onDrawn, dice, 3, 20, 80, random);
    }
    CHECK(vanilla.blocks == std::size_t{40} * 3U * 384U);
    // Not vacuous: on vanilla's shape most of a column is below its
    // threshold and above the bedrock band, and the shortcut answers there.
    CHECK(vanilla.decided * 2U > vanilla.blocks);
    CHECK(random.decided > 0U);
    CHECK(random.decided < random.blocks);
}

TEST_CASE("a threshold that refuses refuses at every block that reads it, kept or not",
          "[surface][program]") {
    // A surface noise no int holds the depth of, and a preliminary surface
    // whose sum with any depth leaves int32: the first block to test the
    // condition refuses, nothing is kept, and so does every later one.
    SECTION("the depth") {
        const ThresholdNoises noises(R"({"firstOctave": -6, "amplitudes": [1e12]})");
        const auto registry = noises.registry();
        Dice dice(77);
        const RuleGraph graph = RuleGraph::resolve(vanillaShape(block("stratum:top")),
                                                   ResourceLocation{"stratum", "t"});
        const Executor executor = Executor::compile(graph, kSeed, geometry(), &registry, kSeaLevel);
        Tally tally;
        walk(executor, dice, 4, 20, 80, tally);
        CHECK(tally.refused > tally.blocks / 2U);
    }
    SECTION("the sum") {
        const ThresholdNoises noises(R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})");
        const auto registry = noises.registry();
        Dice dice(78);
        const RuleGraph graph = RuleGraph::resolve(vanillaShape(block("stratum:top")),
                                                   ResourceLocation{"stratum", "t"});
        const Executor executor = Executor::compile(graph, kSeed, geometry(), &registry, kSeaLevel);
        Tally tally;
        walk(executor, dice, 4, std::numeric_limits<std::int32_t>::max(), 1, tally);
        CHECK(tally.refused > tally.blocks / 2U);
    }
}

TEST_CASE("the threshold shortcut declines whatever memo is not this column's own",
          "[surface][program]") {
    const ThresholdNoises noises(R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})");
    const auto registry = noises.registry();
    const RuleGraph graph =
        RuleGraph::resolve(vanillaShape(block("stratum:top")), ResourceLocation{"stratum", "t"});
    const Executor executor = Executor::compile(graph, kSeed, geometry(), &registry, kSeaLevel);
    const Executor other = Executor::compile(graph, kSeed, geometry(), &registry, kSeaLevel);
    Context at;
    at.x = 5;
    at.z = -9;
    at.preliminarySurface = 64;
    at.y = 300;
    ColumnMemo memo;
    // A fresh memo holds nothing.
    at.y = 20;
    CHECK_FALSE(executor.decidedByThreshold(at, memo).has_value());
    // Once a block of the column has tested the threshold, a block below it
    // in the deepslate-free band is decided — nothing — and one in the
    // deepslate band too.
    at.y = 300;
    CHECK(nameOf(executor.apply(at, memo)) == "stratum:top");
    at.y = 20;
    const auto decided = executor.decidedByThreshold(at, memo);
    REQUIRE(decided.has_value());
    CHECK(*decided == nullptr);
    at.y = -10;
    const auto deep = executor.decidedByThreshold(at, memo);
    REQUIRE(deep.has_value());
    REQUIRE(*deep != nullptr);
    CHECK((*deep)->name.toString() == "minecraft:deepslate");
    // In the gradient's own band the walk draws after the test: not decided.
    at.y = 4;
    CHECK_FALSE(executor.decidedByThreshold(at, memo).has_value());
    // In the bedrock band the walk starts with the bedrock gradient.
    at.y = kMinY + 2;
    CHECK_FALSE(executor.decidedByThreshold(at, memo).has_value());
    // Another column, another executor, another preliminary surface.
    at.y = 20;
    Context elsewhere = at;
    elsewhere.x = 6;
    CHECK_FALSE(executor.decidedByThreshold(elsewhere, memo).has_value());
    CHECK_FALSE(other.decidedByThreshold(at, memo).has_value());
    Context moved = at;
    moved.preliminarySurface = 65;
    CHECK_FALSE(executor.decidedByThreshold(moved, memo).has_value());
    // Above the threshold the walk goes on into the surface rules' block,
    // which the shortcut answers too: the edge ends in a block.
    at.y = 200;
    const auto above = executor.decidedByThreshold(at, memo);
    REQUIRE(above.has_value());
    REQUIRE(*above != nullptr);
    CHECK((*above)->name.toString() == "stratum:top");
}
