// Stratum — the corner cache's memo against the uncached walk, over random graphs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Interpreter::CornerCache's memo (MemoTable) keeps every value it may need
// again in one of three places, decided by the value's key: a corner of an
// `interpolated` argument in that argument's lattice, a box of corners
// anchored at its first cell; a column's value of a node the column memo
// keeps — and a `flat_cache`'s relocated read of such a node, whose key is
// the same — in that column's record; anything else in a hash. This file
// writes graphs at random over all three — 2D nodes and splines a column
// memo keeps, flat_caches over 2D and 3D arguments, `interpolated` over
// corners that read windows, nest other `interpolated`s or refuse,
// find_top_surface scans — and reads them through one cache the ways a chunk
// is read and the ways that drive the memo to its edges:
//
//   * in the filler's order over a chunk's cells, with a read of another
//     root through the chunk's window after every block, as the aquifer's
//     sit between a fill's blocks; and a kernel's run of each row of blocks,
//     where a root has a kernel;
//   * starting from a cell in the middle of the chunk and above it, so that
//     a lattice is anchored there and the cells below and beside it are kept
//     partly or wholly in the hash;
//   * far above the anchor, past the lattice's height;
//   * through a dozen windows, so that the ninth forgets the memo and where
//     it kept everything (CornerCache::forget);
//   * at thousands of columns, past kMaxColumns records and past
//     kMaxValues values, each forgetting the memo whole;
//   * with the memo's epoch at the edge of its wrap.
//
// Every read must give what the walk gives with no memo at all — the
// interpreter's own evaluate(root, at), or a fresh last-cell cache through
// the read's window — the double's bits, or a refusal in the same words.
// Deterministic (a fixed seed through the project's own Xoroshiro); the
// "[.memo-fuzz-long]" case runs many times as many graphs.

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
                stratum::test::tempName("stratum-memo-fuzz")) {
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

private:
    stratum::rng::Xoroshiro128PlusPlus random_;
};

[[nodiscard]] std::string number(double value) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    return {buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] std::string quoted(const std::string& name) {
    return '"' + name + '"';
}

/// The noises every pack defines.
constexpr std::array<std::string_view, 3> kNoises{"memo_a", "memo_b", "memo_c"};

/// Random density functions over the memo's three kinds of keeping.
class Writer {
public:
    explicit Writer(Draw& draw) : draw_(&draw) {}

    /// A noise that refuses wherever |x| or |z| passes @p reach blocks:
    /// its lattice leaves int32 there (NormalNoise::accepts).
    [[nodiscard]] static std::string farNoise(std::int32_t reach, bool flat) {
        const double scale = 2147483648.0 * 16.0 / (1.0181268882175227 * reach);
        return R"({"type":"minecraft:noise","noise":"memo_a","xz_scale":)" + number(scale) +
               R"(,"y_scale":)" + (flat ? "0.0" : "1.0") + "}";
    }

    /// A noise at a scale of its own, so that every one is a node of its
    /// own: @p flat ones are column-invariant, and kept per column.
    [[nodiscard]] std::string noise(bool flat) {
        const double scale = 0.25 + (static_cast<double>(draw_->below(4096)) / 512.0);
        return R"({"type":"minecraft:noise","noise":")" + std::string(draw_->pick(kNoises)) +
               R"(","xz_scale":)" + number(scale) + R"(,"y_scale":)" +
               (flat ? "0.0" : number(0.25 + (static_cast<double>(draw_->below(8)) / 8.0))) + "}";
    }

    /// A column-invariant function: what the column memo keeps.
    [[nodiscard]] std::string flat(std::int32_t depth) {
        switch (depth > 2 ? 0 : draw_->below(12)) {
            case 0:
            case 1:
                return noise(true);
            case 2:
                return R"({"type":"minecraft:shifted_noise","noise":")" +
                       std::string(draw_->pick(kNoises)) + R"(","xz_scale":)" +
                       number(0.5 + (static_cast<double>(draw_->below(64)) / 16.0)) +
                       R"(,"y_scale":0.0,"shift_x":{"type":"minecraft:shift_a","argument":"memo_b"},"shift_y":0.0,"shift_z":{"type":"minecraft:shift_b","argument":"memo_b"}})";
            case 3:
                return R"({"type":"minecraft:flat_cache","argument":)" + flat(depth + 1) + "}";
            case 4:
                // A nested spline over another coordinate as one knot's value.
                return R"({"type":"minecraft:spline","spline":{"coordinate":)" + flat(depth + 1) +
                       R"(,"points":[{"location":-0.5,"value":{"coordinate":)" + flat(depth + 1) +
                       R"(,"points":[{"location":0.0,"value":0.5,"derivative":0.0},)" +
                       R"({"location":0.5,"value":-0.25,"derivative":1.0}]},"derivative":0.5},)" +
                       R"({"location":0.25,"value":-0.75,"derivative":1.5}]}})";
            case 5:
                return R"({"type":"minecraft:add","argument1":)" + flat(depth + 1) +
                       R"(,"argument2":)" + flat(depth + 1) + "}";
            case 6:
                // Refuses far out, on a column of its own: never kept there.
                return farNoise(draw_->pick(kReaches), true);
            case 7:
            case 8:
                return sharedFlat_.empty() ? noise(true) : quoted(draw_->pick(sharedFlat_));
            default:
                return noise(true);
        }
    }

    /// What an `interpolated` blends: 3D and 2D parts, flat_caches over
    /// both, and now and then corners that refuse or nest another
    /// `interpolated`.
    [[nodiscard]] std::string corner(std::int32_t depth) {
        switch (draw_->below(depth > 1 ? 5 : 9)) {
            case 0:
                return noise(false);
            case 1:
                return R"({"type":"minecraft:add","argument1":)" + noise(false) +
                       R"(,"argument2":)" + flat(1) + "}";
            case 2:
                // A flat_cache over a 3D argument, kept in the hash, whose
                // relocated read the window changes.
                return R"({"type":"minecraft:mul","argument1":{"type":"minecraft:flat_cache","argument":)" +
                       noise(false) + R"(},"argument2":)" + flat(1) + "}";
            case 3:
                return R"({"type":"minecraft:flat_cache","argument":)" + flat(1) + "}";
            case 4:
                return sharedCorners_.empty() ? noise(false) : quoted(draw_->pick(sharedCorners_));
            case 5:
                return R"({"type":"minecraft:add","argument1":)" + noise(false) +
                       R"(,"argument2":)" + interpolated(depth + 1) + "}";
            case 6:
                return farNoise(draw_->pick(kReaches), false);
            case 7:
                return R"({"type":"minecraft:range_choice","input":)" + flat(1) +
                       R"(,"min_inclusive":-0.2,"max_exclusive":0.3,"when_in_range":)" +
                       farNoise(draw_->pick(kReaches), false) + R"(,"when_out_of_range":)" +
                       noise(false) + "}";
            default:
                return R"({"type":"minecraft:find_top_surface","density":)" + noise(false) +
                       R"(,"upper_bound":)" + std::to_string(draw_->below(24) - 8) +
                       R"(,"lower_bound":-16,"cell_height":4})";
        }
    }

    [[nodiscard]] std::string interpolated(std::int32_t depth) {
        return R"({"type":"minecraft:interpolated","argument":)" + corner(depth) + "}";
    }

    /// A root: what a fill reads.
    [[nodiscard]] std::string root(std::int32_t depth) {
        if (depth >= 4 || draw_->percent(depth * 20)) {
            switch (draw_->below(4)) {
                case 0:
                    return flat(1);
                case 1:
                    return sharedInterpolated_.empty() ? interpolated(0)
                                                       : quoted(draw_->pick(sharedInterpolated_));
                default:
                    return interpolated(0);
            }
        }
        switch (draw_->below(5)) {
            case 0:
                return R"({"type":"minecraft:add","argument1":)" + root(depth + 1) +
                       R"(,"argument2":)" + root(depth + 1) + "}";
            case 1:
                return R"({"type":"minecraft:min","argument1":)" + root(depth + 1) +
                       R"(,"argument2":)" + root(depth + 1) + "}";
            case 2:
                return R"({"type":"minecraft:range_choice","input":)" + root(depth + 1) +
                       R"(,"min_inclusive":-0.1,"max_exclusive":0.4,"when_in_range":)" +
                       root(depth + 1) + R"(,"when_out_of_range":)" + root(depth + 1) + "}";
            case 3:
                return R"({"type":"minecraft:mul","argument1":)" + flat(1) + R"(,"argument2":)" +
                       root(depth + 1) + "}";
            default:
                return R"({"type":"minecraft:flat_cache","argument":)" + root(depth + 1) + "}";
        }
    }

    void shareFlat(std::string name) { sharedFlat_.push_back(std::move(name)); }

    void shareCorners(std::string name) { sharedCorners_.push_back(std::move(name)); }

    void shareInterpolated(std::string name) { sharedInterpolated_.push_back(std::move(name)); }

private:
    static constexpr std::array<std::int32_t, 4> kReaches{60, 300, 1000, 5000};

    Draw* draw_;
    std::vector<std::string> sharedFlat_;
    std::vector<std::string> sharedCorners_;
    std::vector<std::string> sharedInterpolated_;
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

[[nodiscard]] std::string spell(const Point at) {
    return "(" + std::to_string(at.x) + ", " + std::to_string(at.y) + ", " + std::to_string(at.z) +
           ")";
}

/// What one fuzz run saw.
struct Tally {
    std::size_t graphs = 0;
    std::size_t reads = 0;
    std::size_t refusals = 0;
    std::size_t runs = 0;
    std::size_t batched = 0;
    /// Plans whose cache forgot its values at least once, by what made it.
    std::size_t forgotAtColumns = 0;
    std::size_t forgotAtValues = 0;
    std::size_t wraps = 0;
    std::size_t largest = 0;
    std::size_t differing = 0;
    std::string first = "none";
};

/// The window of chunk (@p chunkX, @p chunkZ), as ChunkFiller::flatCacheWindow.
[[nodiscard]] FlatCacheWindow chunkWindow(std::int32_t chunkX, std::int32_t chunkZ) {
    return FlatCacheWindow{.minX = chunkX * 16,
                           .maxX = (chunkX * 16) + 19,
                           .minZ = chunkZ * 16,
                           .maxZ = (chunkZ * 16) + 19};
}

/// The memo-free walk's outcome of a read: the interpreter's own
/// evaluate(root, at) for a Block read through no window, and otherwise a
/// fresh last-cell cache, which keeps nothing between reads.
class Reference {
public:
    explicit Reference(const Interpreter& interpreter) : interpreter_(&interpreter) {}

    [[nodiscard]] Outcome at(NodeIndex root, Point at, const FlatCacheWindow* window,
                             ReadContext context) const {
        if (window == nullptr && context == ReadContext::Block) {
            return outcomeOf([&] { return interpreter_->evaluate(root, at); });
        }
        Interpreter::CornerCache fresh(interpreter_->cacheSize(), Sharing::LastCell);
        if (window == nullptr) {
            return outcomeOf([&] { return interpreter_->evaluate(root, at, fresh); });
        }
        return outcomeOf([&] { return interpreter_->evaluate(root, at, fresh, *window, context); });
    }

private:
    const Interpreter* interpreter_;
};

/// One read through the cache under test, held to the reference.
class Checker {
public:
    Checker(const Interpreter& interpreter, Tally& tally)
        : interpreter_(&interpreter), reference_(interpreter), tally_(&tally) {}

    void read(NodeIndex root, Point at, Interpreter::CornerCache& cache,
              const FlatCacheWindow* window, ReadContext context, std::string_view plan) {
        ++tally_->reads;
        const Outcome cached = outcomeOf([&] {
            return window == nullptr ? interpreter_->evaluate(root, at, cache)
                                     : interpreter_->evaluate(root, at, cache, *window, context);
        });
        const Outcome fresh = reference_.at(root, at, window, context);
        tally_->refusals += fresh.refused ? 1U : 0U;
        note(cached, fresh, std::string(plan) + " at " + spell(at));
        tally_->largest = std::max(tally_->largest, cache.memoSize());
    }

    /// A kernel's run over @p blocks, each lane held to the reference.
    void run(const BlockKernel& kernel, const BlockKernel::Blocks& blocks,
             Interpreter::CornerCache& cache, BlockKernel::Lanes& lanes) {
        ++tally_->runs;
        if (!interpreter_->evaluate(kernel, blocks, lanes, cache)) {
            return;
        }
        ++tally_->batched;
        for (std::int32_t row = 0; row < blocks.depth; ++row) {
            for (std::int32_t column = 0; column < blocks.width; ++column) {
                const std::size_t lane =
                    (static_cast<std::size_t>(row) * static_cast<std::size_t>(blocks.width)) +
                    static_cast<std::size_t>(column);
                const Point at{
                    .x = blocks.first.x + column, .y = blocks.first.y, .z = blocks.first.z + row};
                const Outcome ran{.refused = false,
                                  .bits = std::bit_cast<std::uint64_t>(lanes.value(lane)),
                                  .message = {}};
                note(ran, reference_.at(kernel.root(), at, nullptr, ReadContext::Block),
                     "run at " + spell(at));
            }
        }
    }

    void note(const Outcome& got, const Outcome& expected, const std::string& where) {
        if (!(got == expected)) {
            if (tally_->differing++ == 0) {
                tally_->first = where + ": memo " + spell(got) + ", walk " + spell(expected);
            }
        }
    }

private:
    const Interpreter* interpreter_;
    Reference reference_;
    Tally* tally_;
};

/// The blocks of the first two cells along x and z of chunk (@p chunkX,
/// @p chunkZ), from @p minY, @p height tall, cell by cell in the filler's
/// order (ChunkFiller::fill), from the cell @p startCell on — the cells
/// before it last — and @p visit at each.
template<typename Visit>
void inFillOrder(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t minY, std::int32_t height,
                 CellGeometry cells, std::size_t startCell, const Visit& visit) {
    std::vector<std::array<std::int32_t, 3>> order;
    const std::int32_t across = std::min(16, 2 * cells.width);
    for (std::int32_t cellZ = 0; cellZ < across; cellZ += cells.width) {
        for (std::int32_t cellX = 0; cellX < across; cellX += cells.width) {
            for (std::int32_t bottom = minY; bottom < minY + height; bottom += cells.height) {
                order.push_back({cellX, bottom, cellZ});
            }
        }
    }
    std::rotate(order.begin(),
                order.begin() + static_cast<std::ptrdiff_t>(startCell % order.size()), order.end());
    for (const auto& [cellX, bottom, cellZ] : order) {
        for (std::int32_t y = bottom; y < std::min(bottom + cells.height, minY + height); ++y) {
            for (std::int32_t z = cellZ; z < std::min(cellZ + cells.width, 16); ++z) {
                for (std::int32_t x = cellX; x < std::min(cellX + cells.width, 16); ++x) {
                    visit(Point{.x = (chunkX * 16) + x, .y = y, .z = (chunkZ * 16) + z});
                }
            }
        }
    }
}

/// Writes random roots into a fresh pack over @p cells and reads each
/// through the plans the file's header lists.
void fuzzPack(Draw& draw, const CellGeometry cells, const std::int32_t graphs, Tally& tally) {
    const FuzzTree tree;
    tree.defineNoise("memo_a", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.defineNoise("memo_b", R"({"firstOctave":-3,"amplitudes":[1.0]})");
    tree.defineNoise("memo_c", R"({"firstOctave":-5,"amplitudes":[1.0,0.0,0.5]})");
    Writer writer(draw);
    for (std::int32_t i = 0; i < 3; ++i) {
        const std::string name = "memo/flat" + std::to_string(i);
        tree.define(name, writer.flat(1));
        writer.shareFlat(name);
    }
    for (std::int32_t i = 0; i < 2; ++i) {
        const std::string name = "memo/corners" + std::to_string(i);
        tree.define(name, writer.corner(1));
        writer.shareCorners(name);
        const std::string blended = "memo/interpolated" + std::to_string(i);
        tree.define(blended, R"({"type":"minecraft:interpolated","argument":")" + name + R"("})");
        writer.shareInterpolated(blended);
    }
    // `wide`: more nodes the column memo keeps than a column record has
    // slots for, so that the rest are kept in the hash.
    std::string wide = writer.noise(true);
    for (std::size_t i = 1; i < Interpreter::CornerCache::kMemoColumnSlots + 8; ++i) {
        std::string sum = R"({"type":"minecraft:add","argument1":)";
        sum += wide;
        sum += R"(,"argument2":)";
        sum += writer.noise(true);
        sum += "}";
        wide = std::move(sum);
    }
    tree.define("memo/wide", wide);
    // `one`: a single node the column memo keeps, so that thousands of
    // columns pass kMaxColumns records long before kMaxValues values.
    tree.define("memo/one", writer.noise(true));
    // `windowed`: a node the column memo keeps whose value the window and
    // the column tell apart — a flat_cache over a 2D sum, read off its
    // column corner, relocated inside a window and not outside — and an
    // `interpolated` over a flat_cache of a 3D noise, which a Detached read
    // reads as its argument.
    tree.define(
        "memo/windowed",
        R"({"type":"minecraft:add","argument1":{"type":"minecraft:flat_cache","argument":{"type":"minecraft:add","argument1":)" +
            writer.noise(true) + R"(,"argument2":)" + writer.flat(1) +
            R"(}},"argument2":{"type":"minecraft:interpolated","argument":{"type":"minecraft:flat_cache","argument":)" +
            writer.noise(false) + "}}}");
    for (std::int32_t i = 0; i < graphs; ++i) {
        tree.define("memo/root" + std::to_string(i), writer.root(0));
    }
    // Two roots a kernel lowers, whatever the draws: arithmetic over
    // `interpolated` nodes, one of them shared.
    constexpr std::int32_t kKernelRoots = 2;
    for (std::int32_t i = 0; i < kKernelRoots; ++i) {
        tree.define(
            "memo/root" + std::to_string(graphs + i),
            R"({"type":"minecraft:add","argument1":"memo/interpolated)" + std::to_string(i) +
                R"(","argument2":{"type":"minecraft:min","argument1":)" + writer.interpolated(0) +
                R"(,"argument2":{"type":"minecraft:y_clamped_gradient","from_y":-8,)" +
                R"("to_y":8,"from_value":1.0,"to_value":-1.0}}})");
    }

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises =
        NoiseRegistry::create(pack, graph.referencedNoises(), draw.below(1000000),
                              stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, cells);
    const auto rootOf = [&graph](const std::string& name) {
        return graph.rootOf(ResourceLocation::parse("minecraft:" + name));
    };
    std::vector<NodeIndex> roots;
    roots.reserve(static_cast<std::size_t>(graphs) + static_cast<std::size_t>(kKernelRoots));
    for (std::int32_t i = 0; i < graphs + kKernelRoots; ++i) {
        roots.push_back(rootOf("memo/root" + std::to_string(i)));
    }
    Checker check(interpreter, tally);
    const std::int32_t minY = -8;
    const std::int32_t height = 16;

    for (std::int32_t i = 0; i < graphs + kKernelRoots; ++i) {
        ++tally.graphs;
        const NodeIndex root = roots[static_cast<std::size_t>(i)];
        const std::int32_t chunkX = draw.below(9) - 4;
        const std::int32_t chunkZ = draw.below(9) - 4;
        const FlatCacheWindow window = chunkWindow(chunkX, chunkZ);
        const std::string where = "root " + std::to_string(i);

        // The fill, from its first cell or from one in the middle, with a
        // read of another root between blocks and, where there is a kernel,
        // its runs over each row of a cell's blocks.
        for (const bool fromMiddle : {false, true}) {
            Interpreter::CornerCache cache(interpreter.cacheSize());
            if (draw.percent(30)) {
                // At the edge of the wrap: the plans below forget the memo
                // and so wrap its epoch.
                cache.advanceMemoEpoch(std::numeric_limits<std::uint32_t>::max() - 1U);
            }
            const NodeIndex between =
                roots[static_cast<std::size_t>(draw.below(graphs + kKernelRoots))];
            const ReadContext context =
                draw.percent(50) ? ReadContext::Detached : ReadContext::Block;
            const std::size_t start = fromMiddle ? static_cast<std::size_t>(draw.below(16)) : 0;
            const std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
            BlockKernel::Lanes lanes;
            inFillOrder(chunkX, chunkZ, minY, height, cells, start, [&](const Point at) {
                if (kernel.has_value() && stratum::javamath::floorMod(at.x, cells.width) == 0 &&
                    stratum::javamath::floorMod(at.z, cells.width) == 0) {
                    const std::int32_t width = std::min(cells.width, 16);
                    check.run(*kernel,
                              BlockKernel::Blocks{.first = at,
                                                  .width = width,
                                                  .depth = std::max(1, std::min(16 / width, width)),
                                                  .lanes = ~0U},
                              cache, lanes);
                }
                check.read(root, at, cache, nullptr, ReadContext::Block, where + " fill");
                check.read(between, at, cache, &window, context, where + " between");
            });
            // Far above the anchor, past the lattice's height.
            for (std::int32_t y = 600; y < 600 + (2 * cells.height); y += 3) {
                check.read(root, Point{.x = (chunkX * 16) + 5, .y = y, .z = (chunkZ * 16) + 6},
                           cache, nullptr, ReadContext::Block, where + " high");
            }
            tally.wraps += cache.memoEpochWraps() > 0U ? 1U : 0U;
        }

        // A dozen windows: the ninth forgets everything, the lattices'
        // anchors and the column slots among it.
        {
            Interpreter::CornerCache cache(interpreter.cacheSize());
            if (draw.percent(50)) {
                cache.advanceMemoEpoch(std::numeric_limits<std::uint32_t>::max());
            }
            for (std::int32_t n = 0; n < 12; ++n) {
                const FlatCacheWindow shifted = chunkWindow(chunkX + n - 6, chunkZ + (n % 3) - 1);
                for (std::int32_t k = 0; k < 24; ++k) {
                    const Point at{.x = (chunkX * 16) + draw.below(20) - 2,
                                   .y = draw.below(height) + minY,
                                   .z = (chunkZ * 16) + draw.below(20) - 2};
                    check.read(root, at, cache, &shifted,
                               draw.percent(70) ? ReadContext::Block : ReadContext::Detached,
                               where + " windows");
                }
            }
            tally.wraps += cache.memoEpochWraps() > 0U ? 1U : 0U;
        }
    }

    // `windowed` through windows that cover its column and that do not, and
    // through none, in both contexts, at columns on and off a column corner
    // and at several heights of each, all on one cache: a value kept under
    // one window's or context's tag must never answer another's.
    {
        Interpreter::CornerCache cache(interpreter.cacheSize());
        const NodeIndex windowed = rootOf("memo/windowed");
        const FlatCacheWindow covering = chunkWindow(0, 0);
        const FlatCacheWindow elsewhere = chunkWindow(5, 5);
        for (std::int32_t c = 0; c < 96; ++c) {
            const std::int32_t x = c % 12;
            const std::int32_t z = (c / 12) * 3;
            for (std::int32_t pass = 0; pass < 6; ++pass) {
                const Point at{.x = x, .y = (pass * 5) - 9, .z = z};
                const ReadContext context =
                    pass % 2 == 0 ? ReadContext::Block : ReadContext::Detached;
                const FlatCacheWindow* window = nullptr;
                if (pass < 2) {
                    window = &covering;
                } else if (pass < 4) {
                    window = &elsewhere;
                }
                check.read(windowed, at, cache, window, context, "windowed");
            }
        }
    }

    // `one` at more columns than kMaxColumns, and `wide` at enough to pass
    // kMaxValues: each cache forgets itself whole, and every read is still
    // the walk's.
    const auto spread = [&](const NodeIndex root, const std::int32_t columns,
                            const std::string& plan) {
        // Whether it forgot, and the most values it held.
        std::pair<bool, std::size_t> seen{false, 0};
        Interpreter::CornerCache cache(interpreter.cacheSize());
        std::size_t previous = 0;
        for (std::int32_t c = 0; c < columns; ++c) {
            const Point at{.x = ((c % 97) * 5) - 200, .y = (c % 7) - 3, .z = ((c / 97) * 3) - 40};
            check.read(root, at, cache, nullptr, ReadContext::Block, plan);
            // The same column at another y, which the column memo answers.
            check.read(root, Point{.x = at.x, .y = at.y + 9, .z = at.z}, cache, nullptr,
                       ReadContext::Block, plan);
            seen.first = seen.first || cache.memoSize() < previous;
            seen.second = std::max(seen.second, cache.memoSize());
            previous = cache.memoSize();
        }
        return seen;
    };
    // `one`, a value a column, past kMaxColumns records: forgotten there,
    // holding far fewer than kMaxValues values.
    const auto [forgotColumns, mostColumns] =
        spread(rootOf("memo/one"),
               static_cast<std::int32_t>(Interpreter::CornerCache::kMemoColumns) + 300, "one");
    tally.forgotAtColumns +=
        forgotColumns && mostColumns <= Interpreter::CornerCache::kMemoColumns ? 1U : 0U;
    // `wide`, well over a hundred values a column, past kMaxValues.
    const auto [forgotValues, mostValues] = spread(rootOf("memo/wide"), 700, "wide");
    tally.forgotAtValues +=
        forgotValues && mostValues > Interpreter::CornerCache::kMemoColumns ? 1U : 0U;
}

void fuzz(std::int64_t seed, std::int32_t packs, std::int32_t graphs, Tally& tally) {
    Draw draw(seed);
    static constexpr std::array<CellGeometry, 4> kCells{
        CellGeometry{.width = 4, .height = 8}, CellGeometry{.width = 8, .height = 4},
        CellGeometry{.width = 3, .height = 5}, CellGeometry{.width = 16, .height = 8}};
    for (std::int32_t pack = 0; pack < packs; ++pack) {
        fuzzPack(draw, kCells[static_cast<std::size_t>(pack) % kCells.size()], graphs, tally);
    }
}

void report(const Tally& tally) {
    CAPTURE(tally.graphs, tally.reads, tally.refusals, tally.runs, tally.batched,
            tally.forgotAtColumns, tally.forgotAtValues, tally.wraps, tally.largest, tally.first);
    CHECK(tally.differing == 0U);
    // Every path was taken: refusals among the reads, kernel runs, both
    // ways of forgetting, the wrap; and never more values than the bound.
    CHECK(tally.refusals > 0U);
    CHECK(tally.batched > 0U);
    CHECK(tally.forgotAtColumns > 0U);
    CHECK(tally.forgotAtValues > 0U);
    CHECK(tally.wraps > 0U);
    CHECK(tally.largest <= Interpreter::CornerCache::kMemoCapacity / 2);
}

} // namespace

TEST_CASE("the corner cache's memo gives the memo-free walk's bits and refusals over random "
          "graphs, in every place it keeps a value",
          "[density][interpreter][memo][memo-fuzz]") {
    Tally tally;
    fuzz(20261030, 4, 3, tally);
    report(tally);
}

TEST_CASE("the corner cache's memo gives the memo-free walk's over many random graphs (long)",
          "[.memo-fuzz-long]") {
    Tally tally;
    fuzz(7331, 200, 8, tally);
    report(tally);
}
