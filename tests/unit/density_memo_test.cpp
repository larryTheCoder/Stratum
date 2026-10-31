// Stratum — the corner cache's memo: when it is used, and what it keeps.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Interpreter::CornerCache keeps, under Sharing::Lattice and with nothing
// substituted (Interpreter::memoizes), every value its evaluations compute
// and may need again — the corner lattice, `flat_cache`'s relocated reads,
// the column memo — and a BlockKernel's lanes run only where it does, since
// they read a cell's corners before the caller's reads in between. This file
// holds both to that one condition, and the memo's epoch to its wrap: a memo
// forgets by moving to the next epoch, and at the wrap, where earlier epochs
// are about to be handed out again, it clears the stamps of every kind of
// place it keeps a value in — of which a wrap on a memo that has kept nothing
// yet tests none.

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
#include <limits>
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

TEST_CASE("a memo whose epoch wraps after it has kept values forgets each kind it kept",
          "[density][interpreter][memo]") {
    const MemoTree tree;
    tree.defineNoise("memo_a", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    // Two noises the column memo keeps (column-invariant, y_scale 0) and
    // their sum, which it keeps too: three slots of every column record.
    tree.define(
        "wrap/sum",
        R"({"type":"minecraft:add","argument1":{"type":"minecraft:noise","noise":"memo_a","xz_scale":1.0,"y_scale":0.0},"argument2":{"type":"minecraft:noise","noise":"memo_a","xz_scale":0.5,"y_scale":0.0}})");
    tree.define("wrap/column",
                R"({"type":"minecraft:noise","noise":"memo_a","xz_scale":1.0,"y_scale":0.0})");
    tree.define(
        "wrap/terrain",
        R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:noise","noise":"memo_a","xz_scale":1.0,"y_scale":1.0}})");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 99,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, kOverworldCells);
    const auto rootOf = [&graph](std::string_view name) {
        return graph.rootOf(ResourceLocation::parse("minecraft:wrap/" + std::string(name)));
    };
    Interpreter::CornerCache cache(interpreter.cacheSize());
    std::size_t differing = 0;
    const auto read = [&](const NodeIndex root, const Point at) {
        differing +=
            bits(interpreter.evaluate(root, at, cache)) != bits(interpreter.evaluate(root, at))
                ? 1U
                : 0U;
    };

    SECTION("column records and the directory naming them") {
        // A record is handed to each new column in turn from 0, and the
        // directory names it by (x, z); the wrap brings back epoch 1, the
        // epoch the first records and their directory entries were stamped
        // in, so both must be cleared there.
        const NodeIndex sum = rootOf("sum");
        const auto column = [](std::int32_t x) { return Point{.x = x, .y = 3, .z = 0}; };
        // Records 0 and 1, epoch 1: (0, 0) and (64, 0), three values each.
        read(sum, column(0));
        read(sum, column(64));
        REQUIRE(cache.memoSize() == 6U);
        // Record 0 again, at the epoch's last value: (128, 0).
        cache.advanceMemoEpoch(std::numeric_limits<std::uint32_t>::max());
        read(sum, column(128));
        REQUIRE(cache.memoSize() == 3U);
        // The wrap, back to epoch 1.
        cache.reset();
        REQUIRE(cache.memoEpochWraps() == 1U);
        // Records 0 and 1 again: (256, 0), then (512, 0) in the record (64, 0)
        // had, whose slots it stamped 1. Then (64, 0), whose directory entry
        // was stamped 1 and named record 1, (512, 0)'s now.
        read(sum, column(256));
        read(sum, column(512));
        CHECK(cache.memoSize() == 6U);
        read(sum, column(64));
        CHECK(cache.memoSize() == 9U);
        // The two columns that share record 1 are told apart by value.
        CHECK(bits(interpreter.evaluate(sum, column(64))) !=
              bits(interpreter.evaluate(sum, column(512))));
        CHECK(differing == 0U);
    }

    SECTION("lattices") {
        // A lattice keeps its corners, and its stamps, until the memo forgets
        // where it keeps things (reset); a memo forgotten whole at
        // kMemoColumns records keeps its lattices, and a wrap there must
        // clear their stamps too, so that a corner stamped 1 before is not
        // held at epoch 1 after. Not a value — a lattice's corner is a pure
        // function of where it is kept — but the work: such a corner would
        // be read rather than made again.
        const NodeIndex terrain = rootOf("terrain");
        const NodeIndex flat = rootOf("column");
        // Cell iy 0: four batches of 8, lattice columns (0, 0), (0, 4),
        // (4, 0) and (4, 4), stamped 1.
        read(terrain, Point{.x = 1, .y = 1, .z = 1});
        REQUIRE(cache.columnBatches() == 4U);
        // The epoch at its last value, then a record for each of
        // kMemoColumns + 1 columns: the last forgets the memo whole, which
        // wraps it, and keeps the lattice.
        cache.advanceMemoEpoch(std::numeric_limits<std::uint32_t>::max());
        for (std::int32_t c = 0;
             c <= static_cast<std::int32_t>(Interpreter::CornerCache::kMemoColumns); ++c) {
            read(flat, Point{.x = 1000 + c, .y = 0, .z = -7});
        }
        REQUIRE(cache.memoEpochWraps() == 1U);
        REQUIRE(cache.memoLattices() == 1U);
        REQUIRE(cache.memoSize() == 1U);
        // Cell (4, 0, 0): columns (4, 0) and (4, 4), stamped 1 before the
        // wrap, and (8, 0) and (8, 4), never stamped. None is held, so four
        // batches of 8 — two fewer would mean the first two read as held.
        read(terrain, Point{.x = 5, .y = 1, .z = 1});
        CHECK(cache.columnBatches() == 8U);
        CHECK(cache.columnCorners() == 64U);
        CHECK(differing == 0U);
    }
}
