// Stratum — the corner cache's lattices together stay inside their bound.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A corner cache's memo keeps each `interpolated` argument's corners in a
// lattice of its own — a chunk's corners across and 512 blocks up, made
// whole when the argument first fills a cell — and every other corner key in
// its hash (Interpreter::MemoTable). The lattices are bounded together
// (CornerCache::kMemoLatticeCorners): an argument that first fills a cell
// with no room left keeps its corners in the hash until the cache is
// forgotten. At 1x1 cells a lattice is 148 257 corners, so the bound holds
// one; at vanilla's 4x8 a few thousand. Wherever an argument's corners are
// kept, every value is the walk's, bit for bit — the uncached walk's and a
// last-cell cache's — and the place each argument's corners are kept is
// fixed from its first fill until the cache is forgotten. A forgotten cache
// gives its lattices' room back whole: read again after a reset, it makes
// the lattices it made the first time, and holds the same corners in them.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

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

/// A worldgen tree written on the fly, removed with the test.
class BoundTree {
public:
    BoundTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-lattice-bound")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    BoundTree(const BoundTree&) = delete;
    BoundTree& operator=(const BoundTree&) = delete;

    ~BoundTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void define(std::string_view registry, std::string_view name, std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::ofstream out(file);
        out << json;
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

constexpr std::array<std::string_view, 4> kRoots{"bound_a", "bound_b", "bound_c", "bound_d"};

/// Four `interpolated`, each over its own argument — a noise scaled and
/// shifted its own way — read at a few cells of a chunk in turn, through a
/// memo cache; every value held to the uncached walk and to a last-cell
/// cache. How many lattices the memo made, and their corners.
struct Filled {
    std::size_t values = 0;
    std::size_t differing = 0;
    std::size_t lattices = 0;
    std::size_t corners = 0;
    std::size_t afterReset = 0;
    std::size_t latticesAgain = 0;
    std::size_t cornersAgain = 0;
};

[[nodiscard]] Filled fill(const CellGeometry cells) {
    const BoundTree tree;
    tree.define("noise", "bound", R"({"firstOctave":-3,"amplitudes":[1.0,0.5]})");
    for (std::size_t i = 0; i < kRoots.size(); ++i) {
        tree.define("density_function", kRoots[i],
                    R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:add",)"
                    R"("argument1":)" +
                        std::to_string(i) +
                        R"(,"argument2":{"type":"minecraft:noise","noise":"minecraft:bound",)"
                        R"("xz_scale":)" +
                        std::to_string(1 + i) + R"(,"y_scale":0.5}}})");
    }
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 77,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, cells);
    Interpreter::CornerCache memo(interpreter.cacheSize(), Sharing::Lattice);
    Interpreter::CornerCache last(interpreter.cacheSize(), Sharing::LastCell);
    Filled filled;
    const auto readAll = [&] {
        for (const std::string_view name : kRoots) {
            const NodeIndex root = graph.rootOf(ResourceLocation::parse(std::string(name)));
            for (std::int32_t y = -8; y < 24; y += 3) {
                for (std::int32_t z = 0; z < 16; z += 5) {
                    for (std::int32_t x = 0; x < 16; x += 3) {
                        const Point at{.x = x, .y = y, .z = z};
                        const double value = interpreter.evaluate(root, at, memo);
                        ++filled.values;
                        if (bits(value) != bits(interpreter.evaluate(root, at)) ||
                            bits(value) != bits(interpreter.evaluate(root, at, last))) {
                            ++filled.differing;
                        }
                    }
                }
            }
        }
    };
    readAll();
    filled.lattices = memo.memoLattices();
    filled.corners = memo.memoLatticeCorners();
    memo.reset();
    filled.afterReset = memo.memoLattices();
    readAll();
    filled.latticesAgain = memo.memoLattices();
    filled.cornersAgain = memo.memoLatticeCorners();
    return filled;
}

} // namespace

TEST_CASE("the corner cache's lattices stay inside their bound, and past it the hash keeps "
          "corners with the walk's values",
          "[density][interpreter][memo]") {
    SECTION("1x1 cells: room for one lattice, and the other arguments' corners in the hash") {
        const Filled filled = fill(CellGeometry{.width = 1, .height = 1});
        CAPTURE(filled.values, filled.lattices, filled.corners);
        CHECK(filled.differing == 0U);
        CHECK(filled.lattices == 1U);
        CHECK(filled.corners == std::size_t{17} * 513U * 17U);
        CHECK(filled.corners <= Interpreter::CornerCache::kMemoLatticeCorners);
        CHECK(filled.afterReset == 0U);
        CHECK(filled.latticesAgain == filled.lattices);
        CHECK(filled.cornersAgain == filled.corners);
    }
    SECTION("4x8 cells: a lattice for every argument") {
        const Filled filled = fill(CellGeometry{.width = 4, .height = 8});
        CAPTURE(filled.values, filled.lattices, filled.corners);
        CHECK(filled.differing == 0U);
        CHECK(filled.lattices == kRoots.size());
        CHECK(filled.corners == kRoots.size() * 5U * 65U * 5U);
        CHECK(filled.afterReset == 0U);
        CHECK(filled.latticesAgain == filled.lattices);
        CHECK(filled.cornersAgain == filled.corners);
    }
}
