// Stratum — the corner cache's memo: when it is used, and what it keeps.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Interpreter::CornerCache keeps, under Sharing::Lattice and with nothing
// substituted (Interpreter::memoizes), every value its evaluations compute
// and may need again — the corner lattice, `flat_cache`'s relocated reads,
// the column memo — and a BlockKernel's lanes run only where it does, since
// they read a cell's corners before the caller's reads in between. This file
// holds both to that one condition.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::CellGeometry;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NodeIndex;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using Sharing = Interpreter::CornerCache::Sharing;
using BlockKernel = Interpreter::BlockKernel;

/// A worldgen tree written on the fly, removed with the test.
class MemoTree {
public:
    MemoTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-density-memo")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    MemoTree(const MemoTree&) = delete;
    MemoTree& operator=(const MemoTree&) = delete;

    ~MemoTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void define(std::string_view name, std::string_view json) const {
        write("density_function", name, json);
    }

    void defineNoise(std::string_view name, std::string_view json) const {
        write("noise", name, json);
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    void write(std::string_view registry, std::string_view name, std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << json;
    }

    std::filesystem::path path_;
};

[[nodiscard]] std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

constexpr CellGeometry kOverworldCells{.width = 4, .height = 8};

/// `root`: an `interpolated` over a 3D noise plus a column-invariant one
/// behind a flat_cache, the three kinds of value the memo keeps.
void defineMemoPack(const MemoTree& tree) {
    tree.defineNoise("memo_a", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.defineNoise("memo_b", R"({"firstOctave":-5,"amplitudes":[1.0]})");
    tree.define("root", R"({"type":"minecraft:add","argument1":{
        "type":"minecraft:interpolated","argument":{"type":"minecraft:add",
            "argument1":{"type":"minecraft:noise","noise":"memo_a","xz_scale":1.0,"y_scale":1.0},
            "argument2":{"type":"minecraft:flat_cache","argument":{
                "type":"minecraft:noise","noise":"memo_b","xz_scale":1.0,"y_scale":0.0}}}},
        "argument2":0.25})");
}

} // namespace

TEST_CASE("the memo and a kernel's lanes are used under one condition: a lattice cache and no "
          "substitution",
          "[density][interpreter][memo]") {
    const MemoTree tree;
    defineMemoPack(tree);
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 31,
                                                       stratum::density::RandomSource::Xoroshiro);
    const NodeIndex root = graph.rootOf(ResourceLocation::parse("minecraft:root"));
    const Point at{.x = 5, .y = 3, .z = -6};
    const BlockKernel::Blocks blocks{
        .first = Point{.x = 4, .y = 3, .z = -8}, .width = 4, .depth = 4, .lanes = ~0U};

    const Interpreter plain(graph, noises, kOverworldCells);
    const double expected = plain.evaluate(root, at);
    const std::optional<BlockKernel> kernel = plain.blockKernel(root);
    REQUIRE(kernel.has_value());

    SECTION("a lattice cache, nothing substituted: both") {
        Interpreter::CornerCache cache(plain.cacheSize());
        CHECK(bits(plain.evaluate(root, at, cache)) == bits(expected));
        CHECK(cache.memoSize() > 0U);
        BlockKernel::Lanes lanes;
        CHECK(plain.evaluate(*kernel, blocks, lanes, cache));
        // Lane (z + 8) * 4 + (x - 4) is `at`.
        CHECK(bits(lanes.value(9)) == bits(expected));
    }

    SECTION("a last-cell cache: neither") {
        Interpreter::CornerCache cache(plain.cacheSize(), Sharing::LastCell);
        CHECK(bits(plain.evaluate(root, at, cache)) == bits(expected));
        CHECK(cache.memoSize() == 0U);
        BlockKernel::Lanes lanes;
        CHECK_FALSE(plain.evaluate(*kernel, blocks, lanes, cache));
        CHECK(bits(plain.evaluate(*kernel, at, cache)) == bits(expected));
        CHECK(cache.memoSize() == 0U);
    }

    SECTION("a substitution, even one this graph never reaches: neither") {
        Interpreter substituted(graph, noises, kOverworldCells);
        stratum::density::UnsettledSubstitutions substitutions;
        substitutions.weirdScaledSampler = 1.0e6;
        substituted.substitute(std::move(substitutions));
        const std::optional<BlockKernel> own = substituted.blockKernel(root);
        REQUIRE(own.has_value());
        Interpreter::CornerCache cache(substituted.cacheSize());
        CHECK(bits(substituted.evaluate(root, at, cache)) == bits(expected));
        CHECK(cache.memoSize() == 0U);
        BlockKernel::Lanes lanes;
        CHECK_FALSE(substituted.evaluate(*own, blocks, lanes, cache));
        CHECK(bits(substituted.evaluate(*own, at, cache)) == bits(expected));
        CHECK(cache.memoSize() == 0U);
    }
}
