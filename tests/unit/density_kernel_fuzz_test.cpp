// Stratum — the block kernel against the interpreter's walk, over random graphs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// density_interpreter_test.cpp pins the kernel (Interpreter::BlockKernel,
// SPEC §4.1) on graphs written by hand. This one writes graphs at random —
// every type a kernel lowers, around `interpolated` nodes reached more than
// once, nested in each other's corners, behind range_choice, over corners
// that refuse — and reads them block by block in the filler's order three
// ways, each through its own CornerCache:
//
//   * the walk, evaluate(root, at, cache), which is the reference;
//   * the kernel one block at a time, evaluate(kernel, at, cache);
//   * the kernel over a run of blocks, evaluate(kernel, blocks, lanes,
//     cache), and where the run declines, the blocks one at a time on that
//     same cache — what ChunkFiller::fill does;
//
// with a read of another root through the chunk's window after each block on
// all three caches, as the aquifer's barrier read sits between a fill's
// blocks. Every block's outcome must be the same on all three: the double's
// bits, or a refusal in the same words. Under a last-cell cache the run must
// always decline, and the same outcomes must follow from the blocks.
//
// Deterministic (a fixed seed through the project's own Xoroshiro) and a few
// seconds long; the "[.kernel-fuzz-long]" case runs many times as many
// graphs, for a verifier's hours rather than CI's.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::CellGeometry;
using stratum::density::EvalError;
using stratum::density::FlatCacheWindow;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NodeIndex;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using stratum::density::ReadContext;
using Sharing = Interpreter::CornerCache::Sharing;
using BlockKernel = Interpreter::BlockKernel;

/// A worldgen tree written on the fly, removed with the test.
class FuzzTree {
public:
    FuzzTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-kernel-fuzz")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    FuzzTree(const FuzzTree&) = delete;
    FuzzTree& operator=(const FuzzTree&) = delete;

    ~FuzzTree() {
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

/// Draws, from the project's own generator.
class Draw {
public:
    explicit Draw(std::int64_t seed) : random_(seed) {}

    /// Uniform in [0, bound).
    [[nodiscard]] std::int32_t below(std::int32_t bound) { return random_.nextInt(bound); }

    [[nodiscard]] bool percent(std::int32_t chance) { return below(100) < chance; }

    template<typename T, std::size_t N>
    [[nodiscard]] const T& pick(const std::array<T, N>& from) {
        return from[static_cast<std::size_t>(below(static_cast<std::int32_t>(N)))];
    }

    [[nodiscard]] const std::string& pick(const std::vector<std::string>& from) {
        return from[static_cast<std::size_t>(below(static_cast<std::int32_t>(from.size())))];
    }

    [[nodiscard]] std::uint32_t bits() { return static_cast<std::uint32_t>(random_.nextInt()); }

private:
    stratum::rng::Xoroshiro128PlusPlus random_;
};

/// Constants exact in decimal, among them both zeros and values whose cubes
/// and products overflow, so infinities and NaN reach min, max, clamp and
/// range_choice.
constexpr std::array<std::string_view, 16> kConstants{
    "0.0",        "-0.0",  "1.0",    "-1.0", "0.5",  "-0.25",  "64.0", "1.5",
    "-1000000.0", "1e300", "-1e300", "3.0",  "0.03", "1.5625", "0.64", "-0.075"};

/// Random density functions over one pack's names.
class Writer {
public:
    explicit Writer(Draw& draw) : draw_(&draw) {}

    /// A function reaching a refusal at every corner farther than about
    /// @p reach blocks from 0 along x or z: a noise whose lattice leaves
    /// int32 there (NormalNoise::accepts).
    [[nodiscard]] static std::string farNoise(std::int32_t reach) {
        // Two octaves' lattice from firstOctave -4 at 1.0181... the second:
        // |x * scale / 16 * 1.0182| < 2^31 within the reach.
        const double scale = 2147483648.0 * 16.0 / (1.0181268882175227 * reach);
        return R"({"type":"minecraft:noise","noise":"fuzz_a","xz_scale":)" + number(scale) +
               R"(,"y_scale":0.0})";
    }

    /// An `interpolated` argument: what is evaluated at the corners.
    [[nodiscard]] std::string corner(std::int32_t depth) {
        const std::int32_t choice = draw_->below(depth > 1 ? 4 : 9);
        switch (choice) {
            case 0:
            case 1:
                return noise();
            case 2:
                return R"({"type":"minecraft:add","argument1":)" + noise() + R"(,"argument2":)" +
                       constant() + "}";
            case 3:
                return R"({"type":"minecraft:mul","argument1":)" + noise() + R"(,"argument2":)" +
                       noise() + "}";
            case 4:
                // A shared argument, which a shared `interpolated` may blend
                // too, so that two read the same corners.
                return sharedCorners_.empty() ? noise() : quoted(draw_->pick(sharedCorners_));
            case 5:
                // Another `interpolated` in the corners, moving its entry.
                return R"({"type":"minecraft:add","argument1":)" + noise() + R"(,"argument2":)" +
                       (sharedInterpolated_.empty() ? interpolated(depth + 1)
                                                    : quoted(draw_->pick(sharedInterpolated_))) +
                       "}";
            case 6:
                return R"({"type":"minecraft:flat_cache","argument":)" + noise() + "}";
            case 7:
                return R"({"type":"minecraft:range_choice","input":)" + gradient() +
                       R"(,"min_inclusive":-0.5,"max_exclusive":0.5,"when_in_range":)" + noise() +
                       R"(,"when_out_of_range":)" + constant() + "}";
            default:
                return farNoise(draw_->pick(kReaches));
        }
    }

    [[nodiscard]] std::string interpolated(std::int32_t depth) {
        return R"({"type":"minecraft:interpolated","argument":)" + corner(depth) + "}";
    }

    /// A block part: what the kernel lowers.
    [[nodiscard]] std::string block(std::int32_t depth) {
        if (depth >= 7 || draw_->percent(depth * 13)) {
            return leaf(depth);
        }
        const std::int32_t choice = draw_->below(20);
        const auto unary = [&](std::string_view type) {
            return R"({"type":"minecraft:)" + std::string(type) + R"(","argument":)" +
                   block(depth + 1) + "}";
        };
        const auto binary = [&](std::string_view type) {
            return R"({"type":"minecraft:)" + std::string(type) + R"(","argument1":)" +
                   block(depth + 1) + R"(,"argument2":)" + block(depth + 1) + "}";
        };
        switch (choice) {
            case 0:
                return unary("abs");
            case 1:
                return unary("square");
            case 2:
                return unary("cube");
            case 3:
                return unary("half_negative");
            case 4:
                return unary("quarter_negative");
            case 5:
                return unary("squeeze");
            case 6:
                return unary("invert");
            case 7:
                return unary("blend_density");
            case 8:
                return unary("cache_once");
            case 9:
                return unary("cache_all_in_cell");
            case 10: {
                static constexpr std::array<std::string_view, 4> kLows{"-1.0", "-0.5", "0.0",
                                                                       "-1e300"};
                static constexpr std::array<std::string_view, 4> kHighs{"1.0", "0.25", "0.0",
                                                                        "64.0"};
                // Its input written inline, as the schema asks: a name is
                // read through a (transparent) cache_once.
                std::string input = block(depth + 1);
                if (input.front() == '"') {
                    input = R"({"type":"minecraft:cache_once","argument":)" + input + "}";
                }
                return R"({"type":"minecraft:clamp","input":)" + input + R"(,"min":)" +
                       std::string(draw_->pick(kLows)) + R"(,"max":)" +
                       std::string(draw_->pick(kHighs)) + "}";
            }
            case 11:
            case 12:
                return binary("add");
            case 13:
                return binary("mul");
            case 14:
                return binary("min");
            case 15:
                return binary("max");
            default: {
                // range_choice over a value that changes inside a cell, so
                // that the lanes of one run part ways.
                const std::string input =
                    draw_->percent(50) ? sharedOrInterpolated(depth) : block(depth + 1);
                static constexpr std::array<std::string_view, 6> kBounds{
                    "-1000000.0", "-0.1", "0.0", "0.05", "0.3", "1e300"};
                const std::string low(draw_->pick(kBounds));
                const std::string high(draw_->pick(kBounds));
                // Now and then a branch over corners that refuse, so that the
                // first block of a run to take it can be any of its blocks.
                const std::string inRange =
                    draw_->percent(25) ? R"({"type":"minecraft:interpolated","argument":)" +
                                             farNoise(draw_->pick(kReaches)) + "}"
                                       : block(depth + 1);
                return R"({"type":"minecraft:range_choice","input":)" + input +
                       R"(,"min_inclusive":)" + low + R"(,"max_exclusive":)" + high +
                       R"(,"when_in_range":)" + inRange + R"(,"when_out_of_range":)" +
                       block(depth + 1) + "}";
            }
        }
    }

    void shareCorners(std::string name) { sharedCorners_.push_back(std::move(name)); }

    void shareInterpolated(std::string name) { sharedInterpolated_.push_back(std::move(name)); }

    void shareBlock(std::string name) { sharedBlocks_.push_back(std::move(name)); }

private:
    static constexpr std::array<std::int32_t, 5> kReaches{24, 40, 100, 200, 300};

    [[nodiscard]] static std::string number(double value) {
        std::array<char, 64> buffer{};
        const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
        return {buffer.data(), static_cast<std::size_t>(written)};
    }

    [[nodiscard]] static std::string quoted(const std::string& name) { return '"' + name + '"'; }

    [[nodiscard]] std::string constant() { return std::string(draw_->pick(kConstants)); }

    [[nodiscard]] std::string noise() {
        static constexpr std::array<std::string_view, 2> kNoises{"fuzz_a", "fuzz_b"};
        static constexpr std::array<std::string_view, 4> kScales{"0.5", "1.0", "2.6666666666666665",
                                                                 "4.0"};
        static constexpr std::array<std::string_view, 3> kYScales{"0.0", "0.5", "1.0"};
        return R"({"type":"minecraft:noise","noise":")" + std::string(draw_->pick(kNoises)) +
               R"(","xz_scale":)" + std::string(draw_->pick(kScales)) + R"(,"y_scale":)" +
               std::string(draw_->pick(kYScales)) + "}";
    }

    [[nodiscard]] std::string gradient() {
        const std::int32_t from = draw_->below(40) - 16;
        std::int32_t to = draw_->below(40) - 16;
        if (to == from) {
            ++to;
        }
        return R"({"type":"minecraft:y_clamped_gradient","from_y":)" + std::to_string(from) +
               R"(,"to_y":)" + std::to_string(to) + R"(,"from_value":)" + constant() +
               R"(,"to_value":)" + constant() + "}";
    }

    [[nodiscard]] std::string sharedOrInterpolated(std::int32_t depth) {
        return !sharedInterpolated_.empty() && draw_->percent(60)
                   ? quoted(draw_->pick(sharedInterpolated_))
                   : interpolated(depth);
    }

    [[nodiscard]] std::string leaf(std::int32_t depth) {
        switch (draw_->below(9)) {
            case 0:
            case 1:
                return constant();
            case 2:
                return draw_->percent(50) ? R"({"type":"minecraft:blend_alpha"})"
                                          : R"({"type":"minecraft:blend_offset"})";
            case 3:
                return R"({"type":"minecraft:cache_2d","argument":)" + constant() + "}";
            case 4:
                return gradient();
            case 5:
                return sharedBlocks_.empty() ? gradient() : quoted(draw_->pick(sharedBlocks_));
            default:
                return sharedOrInterpolated(depth);
        }
    }

    Draw* draw_;
    std::vector<std::string> sharedCorners_;
    std::vector<std::string> sharedInterpolated_;
    std::vector<std::string> sharedBlocks_;
};

/// What one evaluation came to: the double's bits, or a refusal's words.
struct Outcome {
    bool refused = false;
    std::uint64_t bits = 0;
    std::string message;

    [[nodiscard]] bool operator==(const Outcome& other) const = default;
};

template<typename Evaluate>
[[nodiscard]] Outcome outcomeOf(const Evaluate& evaluate) {
    try {
        return Outcome{
            .refused = false, .bits = std::bit_cast<std::uint64_t>(evaluate()), .message = {}};
    } catch (const EvalError& error) {
        return Outcome{.refused = true, .bits = 0, .message = error.what()};
    }
}

[[nodiscard]] std::string spell(const Outcome& outcome) {
    return outcome.refused ? "refused: " + outcome.message : "bits " + std::to_string(outcome.bits);
}

/// What one fuzz run saw.
struct Tally {
    std::size_t graphs = 0;
    std::size_t withoutKernel = 0;
    /// The kernels' ops, all told, and the most one held.
    std::size_t ops = 0;
    std::size_t largest = 0;
    std::size_t blocks = 0;
    std::size_t runs = 0;
    std::size_t batched = 0;
    std::size_t declined = 0;
    std::size_t refusedRuns = 0;
    /// Runs whose first refusing block is not their first block.
    std::size_t refusedMidRun = 0;
    std::size_t refusals = 0;
    std::size_t notFinite = 0;
    std::size_t lastCellRuns = 0;
    std::size_t lastCellBatched = 0;
    std::size_t differing = 0;
    std::string first = "none";
};

/// One run of blocks, as the filler makes them.
struct Run {
    BlockKernel::Blocks blocks;
    /// The window the read between blocks goes through.
    FlatCacheWindow window;
};

/// The window of the chunk @p at is in (ChunkFiller::flatCacheWindow), for
/// a block far from either end of int32.
[[nodiscard]] FlatCacheWindow windowAround(const Point at) {
    const std::int32_t baseX = stratum::javamath::floorDiv(at.x, 16) * 16;
    const std::int32_t baseZ = stratum::javamath::floorDiv(at.z, 16) * 16;
    return FlatCacheWindow{.minX = baseX, .maxX = baseX + 19, .minZ = baseZ, .maxZ = baseZ + 19};
}

/// The runs ChunkFiller::fill makes over a few of a chunk's cells, between
/// y -8 and 16, with the lanes it asks for: every one, or (as for
/// `vein_toggle`) a subset.
[[nodiscard]] std::vector<Run> runsOf(Draw& draw, const CellGeometry cells) {
    std::vector<Run> runs;
    constexpr auto kLanes = static_cast<std::int32_t>(BlockKernel::kLanes);
    for (std::int32_t chunk = 0; chunk < 2; ++chunk) {
        const std::int32_t chunkX = draw.below(41) - 20;
        const std::int32_t chunkZ = draw.below(41) - 20;
        for (std::int32_t pick = 0; pick < 3; ++pick) {
            const std::int32_t across = (16 + cells.width - 1) / cells.width;
            const std::int32_t cellX = draw.below(across) * cells.width;
            const std::int32_t cellZ = draw.below(across) * cells.width;
            const std::int32_t endX = std::min(cellX + cells.width, 16);
            const std::int32_t endZ = std::min(cellZ + cells.width, 16);
            const std::int32_t width = endX - cellX;
            const std::int32_t rows = std::max(1, kLanes / width);
            for (std::int32_t bottom = -8; bottom < 16; bottom += cells.height) {
                for (std::int32_t y = bottom; y < std::min(bottom + cells.height, 16); ++y) {
                    for (std::int32_t z = cellZ; z < endZ; z += rows) {
                        const BlockKernel::Blocks blocks{
                            .first =
                                Point{.x = (chunkX * 16) + cellX, .y = y, .z = (chunkZ * 16) + z},
                            .width = width,
                            .depth = std::min(rows, endZ - z),
                            .lanes = draw.percent(25) ? draw.bits() : ~0U};
                        // The chunk's window as a fill reads through, or
                        // none(), under which a flat_cache relocates nothing
                        // and so changes the corners a last-cell cache keeps.
                        runs.push_back(Run{.blocks = blocks,
                                           .window = draw.percent(50) ? windowAround(blocks.first)
                                                                      : FlatCacheWindow::none()});
                    }
                }
            }
        }
    }
    // A run at the far end of x, whose cell's corners leave int32: refused
    // at the first `interpolated` of every block (cellCorner).
    const std::int32_t width = std::min(cells.width, 16);
    const BlockKernel::Blocks far{
        .first = Point{.x = std::numeric_limits<std::int32_t>::max() - (width - 1), .y = 1, .z = 3},
        .width = width,
        .depth = std::max(1, std::min(kLanes / width, cells.width)),
        .lanes = ~0U};
    runs.push_back(Run{.blocks = far,
                       .window = FlatCacheWindow{.minX = far.first.x,
                                                 .maxX = std::numeric_limits<std::int32_t>::max(),
                                                 .minZ = 0,
                                                 .maxZ = 19}});
    return runs;
}

/// The blocks of @p blocks it asks for, in lane order.
[[nodiscard]] std::vector<std::pair<std::size_t, Point>>
askedBlocks(const BlockKernel::Blocks& blocks) {
    std::vector<std::pair<std::size_t, Point>> asked;
    for (std::int32_t row = 0; row < blocks.depth; ++row) {
        for (std::int32_t column = 0; column < blocks.width; ++column) {
            const std::size_t lane =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(blocks.width)) +
                static_cast<std::size_t>(column);
            if ((blocks.lanes >> lane & 1U) != 0U) {
                asked.emplace_back(lane, Point{.x = blocks.first.x + column,
                                               .y = blocks.first.y,
                                               .z = blocks.first.z + row});
            }
        }
    }
    return asked;
}

/// Writes @p graphs random roots into a fresh pack over @p cells, and checks
/// each root's kernel against the walk, as the file's header says.
void fuzzPack(Draw& draw, const CellGeometry cells, const std::int32_t graphs, Tally& tally) {
    const FuzzTree tree;
    tree.defineNoise("fuzz_a", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.defineNoise("fuzz_b", R"({"firstOctave":-3,"amplitudes":[1.0,0.5]})");
    Writer writer(draw);
    // Shared names, each written from what was shared before it. The first
    // of each is the trap commit 167f3f5 fell into: `interpolated0` over
    // corners its window changes (a flat_cache over a y-varying noise), which
    // the read between blocks fills through the chunk's window; then
    // `interpolated1`, whose corners read `interpolated0` at the
    // neighbouring cells' corners, moving its entry; and `block0`, which
    // reaches `interpolated0` on either side of `interpolated1`. Under a
    // last-cell cache a second read of `interpolated0` there would refill
    // its entry through no window, and take another value than the walk's
    // one read.
    tree.define("fuzz/corners0", R"({"type":"minecraft:add","argument1":{
        "type":"minecraft:noise","noise":"fuzz_a","xz_scale":1.0,"y_scale":1.0},
        "argument2":{"type":"minecraft:flat_cache","argument":{
            "type":"minecraft:noise","noise":"fuzz_b","xz_scale":1.0,"y_scale":1.0}}})");
    writer.shareCorners("fuzz/corners0");
    tree.define("fuzz/interpolated0",
                R"({"type":"minecraft:interpolated","argument":"fuzz/corners0"})");
    writer.shareInterpolated("fuzz/interpolated0");
    tree.define("fuzz/interpolated1", R"({"type":"minecraft:interpolated","argument":{
        "type":"minecraft:add","argument1":{"type":"minecraft:noise","noise":"fuzz_b",
            "xz_scale":0.5,"y_scale":1.0},"argument2":"fuzz/interpolated0"}})");
    writer.shareInterpolated("fuzz/interpolated1");
    tree.define("fuzz/block0", R"({"type":"minecraft:add","argument1":"fuzz/interpolated0",
        "argument2":{"type":"minecraft:add","argument1":"fuzz/interpolated1",
            "argument2":"fuzz/interpolated0"}})");
    writer.shareBlock("fuzz/block0");
    tree.define("fuzz/corners1", writer.corner(2));
    writer.shareCorners("fuzz/corners1");
    for (std::int32_t i = 2; i < 4; ++i) {
        const std::string name = "fuzz/interpolated" + std::to_string(i);
        tree.define(name, writer.interpolated(1));
        writer.shareInterpolated(name);
    }
    tree.define("fuzz/block1", writer.block(3));
    writer.shareBlock("fuzz/block1");
    // What the read between blocks reads: an `interpolated` over a
    // flat_cache, whose corners its window changes, and one the roots share.
    tree.define("fuzz/between", R"({"type":"minecraft:add","argument1":{
        "type":"minecraft:interpolated","argument":{"type":"minecraft:flat_cache","argument":{
            "type":"minecraft:noise","noise":"fuzz_a","xz_scale":1.0,"y_scale":1.0}}},
        "argument2":"fuzz/interpolated0"})");
    for (std::int32_t i = 0; i < graphs; ++i) {
        tree.define("fuzz/root" + std::to_string(i), writer.block(0));
    }

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises =
        NoiseRegistry::create(pack, graph.referencedNoises(), draw.below(1000000),
                              stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, cells);
    const NodeIndex between = graph.rootOf(ResourceLocation::parse("minecraft:fuzz/between"));

    const auto note = [&tally](const std::string& what) {
        if (tally.differing++ == 0) {
            tally.first = what;
        }
    };

    for (std::int32_t i = 0; i < graphs; ++i) {
        ++tally.graphs;
        const NodeIndex root =
            graph.rootOf(ResourceLocation::parse("minecraft:fuzz/root" + std::to_string(i)));
        const std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
        if (!kernel.has_value()) {
            ++tally.withoutKernel;
            continue;
        }
        tally.ops += kernel->size();
        tally.largest = std::max(tally.largest, kernel->size());
        const Sharing sharing = draw.percent(30) ? Sharing::LastCell : Sharing::Lattice;
        Interpreter::CornerCache walkCache(interpreter.cacheSize(), sharing);
        Interpreter::CornerCache kernelCache(interpreter.cacheSize(), sharing);
        Interpreter::CornerCache runCache(interpreter.cacheSize(), sharing);
        BlockKernel::Lanes lanes;
        for (const Run& run : runsOf(draw, cells)) {
            ++tally.runs;
            const bool ran = interpreter.evaluate(*kernel, run.blocks, lanes, runCache);
            if (sharing == Sharing::LastCell) {
                ++tally.lastCellRuns;
                tally.lastCellBatched += ran ? 1U : 0U;
            }
            ++(ran ? tally.batched : tally.declined);
            bool refusedHere = false;
            std::size_t position = 0;
            for (const std::pair<std::size_t, Point>& asked : askedBlocks(run.blocks)) {
                const std::size_t lane = asked.first;
                const Point at = asked.second;
                ++tally.blocks;
                const Outcome viaWalk =
                    outcomeOf([&] { return interpreter.evaluate(root, at, walkCache); });
                const Outcome viaKernel =
                    outcomeOf([&] { return interpreter.evaluate(*kernel, at, kernelCache); });
                const Outcome viaRun =
                    ran ? Outcome{.refused = false,
                                  .bits = std::bit_cast<std::uint64_t>(lanes.value(lane)),
                                  .message = {}}
                        : outcomeOf([&] { return interpreter.evaluate(*kernel, at, runCache); });
                const std::string where = "root " + std::to_string(i) + " at (" +
                                          std::to_string(at.x) + ", " + std::to_string(at.y) +
                                          ", " + std::to_string(at.z) + ") lane " +
                                          std::to_string(lane) + (ran ? " (run)" : " (blocks)");
                if (!(viaKernel == viaWalk)) {
                    note(where + ": kernel " + spell(viaKernel) + ", walk " + spell(viaWalk));
                }
                if (!(viaRun == viaWalk)) {
                    note(where + ": run " + spell(viaRun) + ", walk " + spell(viaWalk));
                }
                if (viaWalk.refused) {
                    ++tally.refusals;
                    if (!refusedHere) {
                        refusedHere = true;
                        ++tally.refusedRuns;
                        tally.refusedMidRun += position > 0 ? 1U : 0U;
                    }
                } else if (!std::isfinite(std::bit_cast<double>(viaWalk.bits))) {
                    ++tally.notFinite;
                }
                // The read a fill makes between blocks, on every cache.
                const auto readBetween = [&](Interpreter::CornerCache& cache) {
                    return outcomeOf([&] {
                        return interpreter.evaluate(between, at, cache, run.window,
                                                    ReadContext::Block);
                    });
                };
                const Outcome walkBetween = readBetween(walkCache);
                if (!(readBetween(kernelCache) == walkBetween) ||
                    !(readBetween(runCache) == walkBetween)) {
                    note(where + ": the read between blocks parts");
                }
                ++position;
            }
            if (ran && refusedHere) {
                note("root " + std::to_string(i) + ": a run stood for a block that refuses");
            }
        }
    }
}

void fuzz(std::int64_t seed, std::int32_t packs, Tally& tally) {
    Draw draw(seed);
    static constexpr std::array<CellGeometry, 5> kCells{
        CellGeometry{.width = 4, .height = 8}, CellGeometry{.width = 8, .height = 4},
        CellGeometry{.width = 4, .height = 4}, CellGeometry{.width = 3, .height = 5},
        CellGeometry{.width = 16, .height = 8}};
    for (std::int32_t pack = 0; pack < packs; ++pack) {
        fuzzPack(draw, kCells[static_cast<std::size_t>(pack) % kCells.size()], 12, tally);
    }
}

void report(const Tally& tally) {
    CAPTURE(tally.graphs, tally.withoutKernel, tally.ops, tally.largest, tally.blocks, tally.runs,
            tally.batched, tally.declined, tally.refusedRuns, tally.refusedMidRun, tally.refusals,
            tally.notFinite, tally.lastCellRuns, tally.first);
    CHECK(tally.differing == 0U);
    // Kernels of some size: range_choice nested in range_choice, several
    // `interpolated` a kernel.
    CHECK(tally.largest > 40U);
    // A last-cell cache never runs blocks together.
    CHECK(tally.lastCellBatched == 0U);
    // Both paths, refusals in the middle of a run among them, were taken.
    CHECK(tally.batched > tally.runs / 4U);
    CHECK(tally.declined > 0U);
    CHECK(tally.refusedMidRun > 0U);
    CHECK(tally.notFinite > 0U);
    CHECK(tally.lastCellRuns > 0U);
    CHECK(tally.graphs - tally.withoutKernel > tally.graphs / 2U);
}

} // namespace

TEST_CASE("a block kernel matches the walk over random graphs one block or one run at a time",
          "[density][interpreter][kernel][kernel-fuzz]") {
    Tally tally;
    fuzz(20261029, 6, tally);
    report(tally);
}

TEST_CASE("a block kernel matches the walk over many random graphs (long)", "[.kernel-fuzz-long]") {
    Tally tally;
    fuzz(4071, 1000, tally);
    report(tally);
}
