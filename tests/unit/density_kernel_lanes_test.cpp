// Stratum — one Lanes shared by block kernels whose constants differ.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A run of a block kernel (Interpreter::evaluate(kernel, blocks, lanes,
// cache)) fills its constants' rows only where the Lanes does not hold that
// kernel's already (BlockKernel::Lanes::constantsOf_): no op writes a
// constant's register, so a run leaves them as it found them. That must hold
// whatever a Lanes is handed: runs of one kernel after another, kernels
// whose constants sit in the same registers with other values, a kernel
// copied, a kernel built again after the first was destroyed, a run that
// declines or asks for no lane — and kernels of two interpreters, whose
// roots may have one node index between them, so that a kernel's number must
// be no other kernel's anywhere in the process (blockKernel), not only in its
// own interpreter. Every lane is held to the kernel one block at a time,
// which fills nothing across calls, by its bits.

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
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::CellGeometry;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NodeIndex;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using BlockKernel = Interpreter::BlockKernel;

/// A worldgen tree written on the fly, removed with the test.
class LanesTree {
public:
    LanesTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-kernel-lanes")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    LanesTree(const LanesTree&) = delete;
    LanesTree& operator=(const LanesTree&) = delete;

    ~LanesTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void define(std::string_view registry, std::string_view name, std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << json;
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    std::filesystem::path path_;
};

/// A root whose kernel holds constants: the same shape every time, so that
/// each kernel's constants take the same registers, with @p first and
/// @p second as their values.
[[nodiscard]] std::string rootWith(std::string_view first, std::string_view second) {
    return R"({"type":"minecraft:add","argument1":{"type":"minecraft:mul","argument1":)" +
           std::string(first) +
           R"(,"argument2":{"type":"minecraft:interpolated","argument":{"type":"minecraft:noise",)"
           R"("noise":"lanes","xz_scale":1.0,"y_scale":1.0}}},"argument2":)" +
           std::string(second) + "}";
}

} // namespace

TEST_CASE("a Lanes shared by kernels whose constants differ gives each its own",
          "[density][interpreter][kernel]") {
    const LanesTree tree;
    tree.define("noise", "lanes", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
    tree.define("density_function", "lanes/a", rootWith("0.5", "-3.0"));
    tree.define("density_function", "lanes/b", rootWith("-2.0", "1e300"));
    tree.define("density_function", "lanes/c", rootWith("0.0", "-0.0"));
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 2026,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 8});
    const auto kernelOf = [&](std::string_view name) {
        const NodeIndex root =
            graph.rootOf(ResourceLocation::parse("minecraft:lanes/" + std::string(name)));
        std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
        REQUIRE(kernel.has_value());
        return *kernel;
    };
    const BlockKernel a = kernelOf("a");
    const BlockKernel b = kernelOf("b");
    std::optional<BlockKernel> c = kernelOf("c");
    // A copy, held apart from the original, is the same kernel.
    const std::vector<BlockKernel> copies{a};

    Interpreter::CornerCache runCache(interpreter.cacheSize());
    Interpreter::CornerCache blockCache(interpreter.cacheSize());
    BlockKernel::Lanes lanes;
    std::size_t lanesRead = 0;
    std::size_t differing = 0;
    std::size_t ran = 0;
    const auto run = [&](const BlockKernel& kernel, const std::int32_t y,
                         const std::uint32_t asked) {
        const BlockKernel::Blocks blocks{
            .first = Point{.x = 4, .y = y, .z = -8}, .width = 4, .depth = 4, .lanes = asked};
        if (!interpreter.evaluate(kernel, blocks, lanes, runCache)) {
            return;
        }
        ++ran;
        for (std::uint32_t lane = 0; lane < 16U; ++lane) {
            if ((asked >> lane & 1U) == 0U) {
                continue;
            }
            const Point at{.x = 4 + static_cast<std::int32_t>(lane % 4U),
                           .y = y,
                           .z = -8 + static_cast<std::int32_t>(lane / 4U)};
            ++lanesRead;
            differing +=
                std::bit_cast<std::uint64_t>(lanes.value(lane)) !=
                        std::bit_cast<std::uint64_t>(interpreter.evaluate(kernel, at, blockCache))
                    ? 1U
                    : 0U;
        }
    };
    for (std::int32_t y = -16; y < 16; ++y) {
        // A run asking for nothing first, which fills nothing; then each
        // kernel in turn, and a copy, which is the same kernel.
        run(b, y, 0U);
        run(a, y, ~0U);
        run(copies.front(), y, 0x0F0FU);
        run(b, y, ~0U);
        run(*c, y, 0xFFFFU);
        run(a, y, 0x8001U);
        if (y == 0) {
            // A kernel built again where the last stood, maybe at its
            // address: a new one all the same.
            c.reset();
            c = kernelOf("b");
        }
    }
    CHECK(ran > 100U);
    CHECK(lanesRead > 1000U);
    CHECK(differing == 0U);
}

namespace {

/// One root of rootWith's shape in a pack of its own, and an interpreter of
/// it: every such pack resolves to the same nodes in the same order.
struct OwnPack {
    LanesTree tree;
    std::optional<Pack> pack;
    std::optional<Graph> graph;
    std::optional<NoiseRegistry> noises;
    std::optional<Interpreter> interpreter;
    NodeIndex root = 0;

    OwnPack(std::string_view first, std::string_view second) {
        tree.define("noise", "lanes", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
        tree.define("density_function", "lanes/root", rootWith(first, second));
        pack.emplace(tree.pack());
        graph.emplace(Graph::resolveAll(*pack));
        noises.emplace(NoiseRegistry::create(*pack, graph->referencedNoises(), 2026,
                                             stratum::density::RandomSource::Xoroshiro));
        interpreter.emplace(*graph, *noises, CellGeometry{.width = 4, .height = 8});
        root = graph->rootOf(ResourceLocation::parse("minecraft:lanes/root"));
    }
};

} // namespace

TEST_CASE("a Lanes shared by two interpreters' kernels of one root index gives each its own",
          "[density][interpreter][kernel]") {
    const OwnPack one("0.5", "-3.0");
    const OwnPack two("-2.0", "7.0");
    // The shape: one node index for both roots, so that a kernel number made
    // of anything an interpreter has of its own, such as the root, would be
    // one number for both kernels.
    REQUIRE(one.root == two.root);
    const std::optional<BlockKernel> a = one.interpreter->blockKernel(one.root);
    const std::optional<BlockKernel> b = two.interpreter->blockKernel(two.root);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    Interpreter::CornerCache runsOne(one.interpreter->cacheSize());
    Interpreter::CornerCache runsTwo(two.interpreter->cacheSize());
    Interpreter::CornerCache blocksOne(one.interpreter->cacheSize());
    Interpreter::CornerCache blocksTwo(two.interpreter->cacheSize());
    BlockKernel::Lanes lanes;
    std::size_t lanesRead = 0;
    std::size_t differing = 0;
    // Lanes where the two kernels' values differ: where one kernel's
    // constants in the other's run would show.
    std::size_t apart = 0;
    const auto blockAt = [](const std::int32_t y, const std::uint32_t lane) {
        return Point{.x = 4 + static_cast<std::int32_t>(lane % 4U),
                     .y = y,
                     .z = -8 + static_cast<std::int32_t>(lane / 4U)};
    };
    const auto run = [&](const OwnPack& own, const BlockKernel& kernel,
                         Interpreter::CornerCache& runs, Interpreter::CornerCache& blocks,
                         const std::int32_t y) {
        const BlockKernel::Blocks blocksRun{
            .first = Point{.x = 4, .y = y, .z = -8}, .width = 4, .depth = 4, .lanes = ~0U};
        REQUIRE(own.interpreter->evaluate(kernel, blocksRun, lanes, runs));
        for (std::uint32_t lane = 0; lane < 16U; ++lane) {
            ++lanesRead;
            differing += std::bit_cast<std::uint64_t>(lanes.value(lane)) !=
                                 std::bit_cast<std::uint64_t>(
                                     own.interpreter->evaluate(kernel, blockAt(y, lane), blocks))
                             ? 1U
                             : 0U;
        }
    };
    for (std::int32_t y = -16; y < 16; ++y) {
        // One kernel, then the other, on the one Lanes.
        run(one, *a, runsOne, blocksOne, y);
        run(two, *b, runsTwo, blocksTwo, y);
        for (std::uint32_t lane = 0; lane < 16U; ++lane) {
            apart += std::bit_cast<std::uint64_t>(
                         one.interpreter->evaluate(*a, blockAt(y, lane), blocksOne)) !=
                             std::bit_cast<std::uint64_t>(
                                 two.interpreter->evaluate(*b, blockAt(y, lane), blocksTwo))
                         ? 1U
                         : 0U;
        }
    }
    // 32 heights of 16 lanes, run by each kernel.
    CHECK(lanesRead == std::size_t{2} * 32U * 16U);
    CHECK(apart == std::size_t{32} * 16U);
    CHECK(differing == 0U);
}
