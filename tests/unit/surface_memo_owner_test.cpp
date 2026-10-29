// Stratum — a column memo answers only for the executor that filled it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// surface::ColumnMemo keeps a column's surface depth, secondary depth and
// noises for the next block of the column. Those are each EXECUTOR's own —
// another compile over other noises has other values at the same (x, z) — so
// a memo keyed by the column alone, handed to two executors in turn, would
// answer one with what the other kept. It is bound to the executor too.
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
#include <string>
#include <string_view>
#include <vector>

namespace {

using stratum::data::ResourceLocation;
using stratum::surface::Context;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

/// `minecraft:surface` with @p amplitudes and `minecraft:surface_secondary`.
class DepthNoises {
public:
    explicit DepthNoises(std::string_view amplitudes)
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-surface-memo-owner-test")) {
        const std::filesystem::path noiseDir = path_ / "noise";
        std::filesystem::create_directories(noiseDir);
        std::ofstream surface(noiseDir / "surface.json");
        surface << R"({"firstOctave": -6, "amplitudes": )" << amplitudes << "}";
        std::ofstream secondary(noiseDir / "surface_secondary.json");
        secondary << R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 0.0, 1.0]})";
    }

    DepthNoises(const DepthNoises&) = delete;
    DepthNoises& operator=(const DepthNoises&) = delete;
    DepthNoises(DepthNoises&&) = delete;
    DepthNoises& operator=(DepthNoises&&) = delete;

    ~DepthNoises() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::density::NoiseRegistry registry() const {
        const std::vector<ResourceLocation> names{
            ResourceLocation::parse("minecraft:surface"),
            ResourceLocation::parse("minecraft:surface_secondary")};
        return stratum::density::NoiseRegistry::create(stratum::data::Pack::openWorldgenTree(path_),
                                                       names, 42,
                                                       stratum::density::RandomSource::Xoroshiro);
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] nlohmann::json when(const nlohmann::json& condition, const std::string& name) {
    return nlohmann::json{
        {"type", "minecraft:condition"},
        {"if_true", condition},
        {"then_run", {{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}}}};
}

/// Three conditions over a column's kept reads: the depth through `hole`
/// and `above_preliminary_surface`, the depth and the secondary depth
/// through a `stone_depth` that adds both.
[[nodiscard]] RuleGraph depthTree() {
    return RuleGraph::resolve(
        nlohmann::json{
            {"type", "minecraft:sequence"},
            {"sequence",
             {when({{"type", "minecraft:hole"}}, "minecraft:water"),
              when({{"type", "minecraft:stone_depth"},
                    {"offset", 0},
                    {"surface_type", "floor"},
                    {"add_surface_depth", true},
                    {"secondary_depth_range", 6}},
                   "minecraft:dirt"),
              when({{"type", "minecraft:above_preliminary_surface"}}, "minecraft:grass_block")}}},
        ResourceLocation::parse("stratum:test"));
}

[[nodiscard]] std::string nameOf(const stratum::settings::BlockState* placed) {
    return placed == nullptr ? std::string("nothing") : placed->name.toString();
}

} // namespace

TEST_CASE("one column memo handed to two executors in turn answers each with its own reads",
          "[surface][memo]") {
    const DepthNoises tame("[1.0, 1.0, 1.0]");
    const DepthNoises wild("[4.0, 0.5, 3.0]");
    const auto tameNoises = tame.registry();
    const auto wildNoises = wild.registry();
    const RuleGraph graph = depthTree();
    const auto geometry = stratum::settings::NoiseGeometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
    const Executor first = Executor::compile(graph, 42, geometry, &tameNoises);
    const Executor second = Executor::compile(graph, 42, geometry, &wildNoises);

    stratum::surface::ColumnMemo shared;
    std::size_t parted = 0;
    std::size_t compared = 0;
    for (std::int32_t x = -48; x < 48; ++x) {
        for (const std::int32_t z : {-5, 11}) {
            Context block;
            block.x = x;
            block.z = z;
            block.preliminarySurface = 64;
            // Down the column, the two executors interleaved block by block,
            // each with the one memo: every answer must be the memo-free one.
            for (std::int32_t y = 70; y >= 50; --y) {
                block.y = y;
                block.stoneDepthAbove = 1 + ((70 - y) % 9);
                const std::string fromFirst = nameOf(first.apply(block));
                const std::string fromSecond = nameOf(second.apply(block));
                CAPTURE(x, y, z);
                REQUIRE(nameOf(first.apply(block, shared)) == fromFirst);
                REQUIRE(nameOf(second.apply(block, shared)) == fromSecond);
                parted += fromFirst != fromSecond ? 1U : 0U;
                ++compared;
            }
        }
    }
    CHECK(compared == 96U * 2U * 21U);
    // The executors disagree on plenty of these blocks, so a memo that
    // answered one with the other's reads would have been caught.
    CHECK(parted > 200U);
}

TEST_CASE("a copy of an executor shares its column memo, and answers it alike", "[surface][memo]") {
    const DepthNoises tame("[1.0, 1.0, 1.0]");
    const auto noises = tame.registry();
    const RuleGraph graph = depthTree();
    const auto geometry = stratum::settings::NoiseGeometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
    const Executor original = Executor::compile(graph, 42, geometry, &noises);
    const Executor copy = original; // NOLINT(performance-unnecessary-copy-initialization)
    stratum::surface::ColumnMemo shared;
    for (std::int32_t x = -20; x < 20; ++x) {
        Context block;
        block.x = x;
        block.z = 3;
        block.preliminarySurface = 64;
        for (std::int32_t y = 70; y >= 50; --y) {
            block.y = y;
            block.stoneDepthAbove = 1 + ((70 - y) % 5);
            CAPTURE(x, y);
            const auto* expected = original.apply(block);
            REQUIRE(original.apply(block, shared) == expected);
            REQUIRE(copy.apply(block, shared) == expected);
        }
    }
}
