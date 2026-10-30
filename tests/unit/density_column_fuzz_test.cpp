// Stratum — column batches of a lattice's corners against the walk, over random graphs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A block kernel's `interpolated` reached on every path evaluates its
// argument's corners a lattice column at a time (Interpreter::batchedCorner,
// evaluateLanes): every node over every lane, each noise sampled at the
// column's points at once. This file writes corner arguments at random over
// every node type a batch takes — noise and shift at every y scale, shifted
// noise over column-invariant shifts and over shifts that part the lanes,
// weird_scaled_sampler of both mappers, old_blended_noise, the arithmetic,
// range_choice whose lanes part ways, flat_cache, splines down the column
// and splines a column shares, column-invariant parts — and among them
// corners that refuse: a noise past its lattice above some height, so that a
// batch reaches a refusing corner in its middle, and far out on x, so that
// one refuses at every lane. It fills chunks with them in ChunkFiller::fill's
// order, a run of the kernel per cell row and the blocks one at a time where
// the run declines, through a memo cache (where the batches run), and holds
// every block to the same block read through a last-cell cache, which keeps
// no lattice and so batches nothing, and every few blocks to the uncached
// walk: the double's bits, or a refusal in the same words, and the chunk's
// first refusal at the same block. Some chunks are read first at a few
// blocks through the walk, which keeps those cells' corners without a batch,
// so that a run up a column stops at a corner already held; one chunk is
// taller than a batch holds; one fills a cache whose memo is about to forget
// itself; one starts at the wrap of the batches' epoch.
//
// Deterministic (a fixed seed through the project's own Xoroshiro) and a few
// seconds long; "[.column-fuzz-long]" runs many times as many packs.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
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
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NodeIndex;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using Sharing = Interpreter::CornerCache::Sharing;
using BlockKernel = Interpreter::BlockKernel;

/// A worldgen tree written on the fly, removed with the test.
class FuzzTree {
public:
    FuzzTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-column-fuzz")) {
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

[[nodiscard]] std::string number(double value) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    return {buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] std::string quoted(const std::string& name) {
    return '"' + name + '"';
}

/// The noises every pack defines: one octave, two, and three of them.
constexpr std::array<std::string_view, 3> kNoises{"column_a", "column_b", "column_c"};

/// Constants exact in decimal, among them both zeros and values whose cubes
/// and products overflow, so infinities and NaN reach the lanes.
constexpr std::array<std::string_view, 12> kConstants{"0.0",        "-0.0",  "1.0",  "-1.0",
                                                      "0.5",        "-0.25", "64.0", "1.5",
                                                      "-1000000.0", "1e300", "3.0",  "0.03"};

/// Random corner arguments over every node type a column batch takes.
class Writer {
public:
    explicit Writer(Draw& draw) : draw_(&draw) {}

    /// A noise that refuses wherever |y| passes about @p height blocks: its
    /// lattice leaves int32 there (NormalNoise::accepts) — in the middle of
    /// a column, so that a batch reaches it at some lanes and not others.
    [[nodiscard]] static std::string tallNoise(std::int32_t height) {
        const double scale = 2147483648.0 * 16.0 / (1.0181268882175227 * height);
        return R"({"type":"minecraft:noise","noise":"column_a","xz_scale":1.0,"y_scale":)" +
               number(scale) + "}";
    }

    /// A noise that refuses wherever |x| or |z| passes @p reach blocks,
    /// column-invariant (@p flat) or not: at every lane of a batch at once.
    [[nodiscard]] static std::string farNoise(std::int32_t reach, bool flat) {
        const double scale = 2147483648.0 * 16.0 / (1.0181268882175227 * reach);
        return R"({"type":"minecraft:noise","noise":"column_a","xz_scale":)" + number(scale) +
               R"(,"y_scale":)" + (flat ? "0.0" : "1.0") + "}";
    }

    /// A noise of some scales; @p flat ones are column-invariant.
    [[nodiscard]] std::string noise(bool flat) {
        static constexpr std::array<std::string_view, 5> kXz{"0.5", "1.0", "2.6666666666666665",
                                                             "4.0", "25.0"};
        static constexpr std::array<std::string_view, 6> kY{"0.5", "1.0", "0.6666666666666666",
                                                            "8.0", "0.3", "-1.0"};
        return R"({"type":"minecraft:noise","noise":")" + std::string(draw_->pick(kNoises)) +
               R"(","xz_scale":)" + std::string(draw_->pick(kXz)) + R"(,"y_scale":)" +
               (flat ? std::string(draw_->percent(50) ? "0.0" : "-0.0")
                     : std::string(draw_->pick(kY))) +
               "}";
    }

    /// A column-invariant function, which a batch reads once through the
    /// walk.
    [[nodiscard]] std::string flat(std::int32_t depth) {
        switch (depth > 2 ? 0 : draw_->below(9)) {
            case 0:
            case 1:
                return noise(true);
            case 2:
                return R"({"type":"minecraft:shifted_noise","noise":")" +
                       std::string(draw_->pick(kNoises)) +
                       R"(","xz_scale":1.0,"y_scale":0.0,"shift_x":{"type":"minecraft:flat_cache","argument":{"type":"minecraft:cache_2d","argument":{"type":"minecraft:shift_a","argument":"column_b"}}},"shift_y":0.0,"shift_z":{"type":"minecraft:flat_cache","argument":{"type":"minecraft:cache_2d","argument":{"type":"minecraft:shift_b","argument":"column_b"}}}})";
            case 3:
                return R"({"type":"minecraft:cache_2d","argument":)" + flat(depth + 1) + "}";
            case 4:
                return spline(flat(depth + 1));
            case 5:
                return R"({"type":"minecraft:mul","argument1":)" + flat(depth + 1) +
                       R"(,"argument2":)" + constant() + "}";
            case 6:
                // Refuses far out, at every lane of the column.
                return farNoise(draw_->pick(kReaches), true);
            case 7:
                return R"({"type":"minecraft:range_choice","input":)" + noise(true) +
                       R"(,"min_inclusive":-0.2,"max_exclusive":0.3,"when_in_range":)" +
                       farNoise(draw_->pick(kReaches), true) + R"(,"when_out_of_range":)" +
                       noise(true) + "}";
            default:
                return constant();
        }
    }

    /// What an `interpolated` blends: every type a batch takes.
    [[nodiscard]] std::string corner(std::int32_t depth) {
        if (depth >= 5 || draw_->percent(depth * 15)) {
            switch (draw_->below(5)) {
                case 0:
                case 1:
                    return noise(false);
                case 2:
                    return gradient();
                case 3:
                    return flat(1);
                default:
                    return sharedCorners_.empty() ? noise(false)
                                                  : quoted(draw_->pick(sharedCorners_));
            }
        }
        const auto unary = [&](std::string_view type) {
            return R"({"type":"minecraft:)" + std::string(type) + R"(","argument":)" +
                   corner(depth + 1) + "}";
        };
        const auto binary = [&](std::string_view type) {
            return R"({"type":"minecraft:)" + std::string(type) + R"(","argument1":)" +
                   corner(depth + 1) + R"(,"argument2":)" + corner(depth + 1) + "}";
        };
        static constexpr std::array<std::string_view, 9> kUnary{
            "abs",     "square", "cube",          "half_negative", "quarter_negative",
            "squeeze", "invert", "blend_density", "cache_once"};
        static constexpr std::array<std::string_view, 4> kBinary{"add", "mul", "min", "max"};
        switch (draw_->below(18)) {
            case 0:
                return noise(false);
            case 1:
                return R"({"type":"minecraft:shift","argument":")" +
                       std::string(draw_->pick(kNoises)) + R"("})";
            case 2:
                // Shifts a column shares: one x and z at every lane.
                return R"({"type":"minecraft:shifted_noise","noise":")" +
                       std::string(draw_->pick(kNoises)) +
                       R"(","xz_scale":0.25,"y_scale":0.5,"shift_x":)" + flat(2) +
                       R"(,"shift_y":)" + corner(depth + 1) + R"(,"shift_z":)" + flat(2) + "}";
            case 3:
                // Shifts that part the lanes' x and z.
                return R"({"type":"minecraft:shifted_noise","noise":")" +
                       std::string(draw_->pick(kNoises)) +
                       R"(","xz_scale":1.0,"y_scale":1.0,"shift_x":)" + noise(false) +
                       R"(,"shift_y":0.0,"shift_z":)" + corner(depth + 1) + "}";
            case 4: {
                static constexpr std::array<std::string_view, 2> kMappers{"type_1", "type_2"};
                return R"({"type":"minecraft:weird_scaled_sampler","rarity_value_mapper":")" +
                       std::string(draw_->pick(kMappers)) + R"(","noise":")" +
                       std::string(draw_->pick(kNoises)) + R"(","input":)" + corner(depth + 1) +
                       "}";
            }
            case 5: {
                static constexpr std::array<std::string_view, 3> kXz{"0.25", "1.0", "0.003"};
                static constexpr std::array<std::string_view, 3> kY{"0.125", "0.25", "1.0"};
                static constexpr std::array<std::string_view, 3> kSmear{"8.0", "4.0", "0.0"};
                return R"({"type":"minecraft:old_blended_noise","xz_scale":)" +
                       std::string(draw_->pick(kXz)) + R"(,"y_scale":)" +
                       std::string(draw_->pick(kY)) + R"(,"xz_factor":80.0,"y_factor":160.0,)" +
                       R"("smear_scale_multiplier":)" + std::string(draw_->pick(kSmear)) + "}";
            }
            case 6:
                return gradient();
            case 7:
            case 8:
                return unary(draw_->pick(kUnary));
            case 9:
            case 10:
                return binary(draw_->pick(kBinary));
            case 11: {
                static constexpr std::array<std::string_view, 4> kLows{"-1.0", "-0.5", "0.0",
                                                                       "-1e300"};
                static constexpr std::array<std::string_view, 4> kHighs{"1.0", "0.25", "0.0",
                                                                        "64.0"};
                std::string input = corner(depth + 1);
                if (input.front() == '"') {
                    input = R"({"type":"minecraft:cache_once","argument":)" + input + "}";
                }
                return R"({"type":"minecraft:clamp","input":)" + input + R"(,"min":)" +
                       std::string(draw_->pick(kLows)) + R"(,"max":)" +
                       std::string(draw_->pick(kHighs)) + "}";
            }
            case 12: {
                // Lanes that part ways down the column.
                static constexpr std::array<std::string_view, 5> kBounds{"-1000000.0", "-0.1",
                                                                         "0.0", "0.3", "1e300"};
                const std::string input = draw_->percent(50) ? gradient() : corner(depth + 1);
                return R"({"type":"minecraft:range_choice","input":)" + input +
                       R"(,"min_inclusive":)" + std::string(draw_->pick(kBounds)) +
                       R"(,"max_exclusive":)" + std::string(draw_->pick(kBounds)) +
                       R"(,"when_in_range":)" + corner(depth + 1) + R"(,"when_out_of_range":)" +
                       corner(depth + 1) + "}";
            }
            case 13:
                // Read at its column's corner, the same for every lane.
                return R"({"type":"minecraft:flat_cache","argument":)" + corner(depth + 1) + "}";
            case 14:
                // Down the column, lane by lane through the walk.
                return spline(corner(depth + 1));
            case 15:
                // A corner that refuses above some height, at some lanes of
                // a column and not at others; now and then behind a branch
                // only the higher lanes take.
                if (draw_->percent(50)) {
                    return tallNoise(draw_->pick(kHeights));
                }
                return R"({"type":"minecraft:range_choice","input":)" + gradient() +
                       R"(,"min_inclusive":0.5,"max_exclusive":1e300,"when_in_range":)" +
                       tallNoise(draw_->pick(kHeights)) + R"(,"when_out_of_range":)" +
                       corner(depth + 1) + "}";
            case 16:
                return R"({"type":"minecraft:add","argument1":)" + corner(depth + 1) +
                       R"(,"argument2":)" + flat(1) + "}";
            default:
                return sharedCorners_.empty() ? noise(false) : quoted(draw_->pick(sharedCorners_));
        }
    }

    [[nodiscard]] std::string interpolated(std::int32_t depth) {
        return R"({"type":"minecraft:interpolated","argument":)" + corner(depth) + "}";
    }

    /// A root a kernel lowers: `interpolated` on every path and behind a
    /// branch, as vanilla's final_density reads its terrain and its noodle
    /// caves.
    [[nodiscard]] std::string root() {
        std::string terrain = sharedOrInterpolated();
        switch (draw_->below(4)) {
            case 0:
                return terrain;
            case 1:
                return R"({"type":"minecraft:add","argument1":)" + terrain + R"(,"argument2":)" +
                       sharedOrInterpolated() + "}";
            case 2:
                return R"({"type":"minecraft:min","argument1":{"type":"minecraft:squeeze","argument":{"type":"minecraft:mul","argument1":0.64,"argument2":)" +
                       terrain + R"(}},"argument2":{"type":"minecraft:range_choice","input":)" +
                       sharedOrInterpolated() +
                       R"(,"min_inclusive":-1000000.0,"max_exclusive":0.0,"when_in_range":64.0,"when_out_of_range":{"type":"minecraft:add","argument1":)" +
                       sharedOrInterpolated() +
                       R"(,"argument2":{"type":"minecraft:abs","argument":)" +
                       sharedOrInterpolated() + "}}}}";
            default:
                return R"({"type":"minecraft:mul","argument1":)" + gradient() + R"(,"argument2":)" +
                       terrain + "}";
        }
    }

    void shareCorners(std::string name) { sharedCorners_.push_back(std::move(name)); }

    void shareInterpolated(std::string name) { sharedInterpolated_.push_back(std::move(name)); }

private:
    static constexpr std::array<std::int32_t, 4> kReaches{60, 300, 1000, 5000};
    static constexpr std::array<std::int32_t, 4> kHeights{10, 30, 70, 200};

    [[nodiscard]] std::string constant() { return std::string(draw_->pick(kConstants)); }

    [[nodiscard]] std::string gradient() {
        const std::int32_t from = draw_->below(160) - 40;
        std::int32_t to = draw_->below(160) - 40;
        if (to == from) {
            ++to;
        }
        return R"({"type":"minecraft:y_clamped_gradient","from_y":)" + std::to_string(from) +
               R"(,"to_y":)" + std::to_string(to) + R"(,"from_value":)" + constant() +
               R"(,"to_value":)" + constant() + "}";
    }

    /// A spline over @p coordinate, with a nested spline as one knot.
    [[nodiscard]] static std::string spline(const std::string& coordinate) {
        return R"({"type":"minecraft:spline","spline":{"coordinate":)" + coordinate +
               R"(,"points":[{"location":-0.5,"value":{"coordinate":)" + coordinate +
               R"(,"points":[{"location":0.0,"value":0.5,"derivative":0.0},)" +
               R"({"location":0.5,"value":-0.25,"derivative":1.0}]},"derivative":0.5},)" +
               R"({"location":0.25,"value":-0.75,"derivative":1.5}]}})";
    }

    [[nodiscard]] std::string sharedOrInterpolated() {
        return !sharedInterpolated_.empty() && draw_->percent(50)
                   ? quoted(draw_->pick(sharedInterpolated_))
                   : interpolated(0);
    }

    Draw* draw_;
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

/// What one fuzz run saw.
struct Tally {
    std::size_t roots = 0;
    std::size_t withoutKernel = 0;
    std::size_t chunks = 0;
    std::size_t blocks = 0;
    std::size_t walked = 0;
    std::size_t refusedChunks = 0;
    /// Chunks whose first refusal came after a batch had declined.
    std::size_t refusedAfterDecline = 0;
    std::uint64_t batches = 0;
    std::uint64_t corners = 0;
    std::uint64_t declines = 0;
    /// Batches of more corners than a lattice column of 33 holds.
    std::size_t tallChunks = 0;
    std::size_t memoForgets = 0;
    std::size_t differing = 0;
    std::string first = "none";
};

/// How a chunk is filled.
struct Fill {
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    std::int32_t minY = 0;
    std::int32_t topY = 0;
    /// Blocks read through the walk on the memo cache first.
    std::int32_t preReads = 0;
    /// Columns read through the walk to bring the memo near kMaxValues.
    bool nearlyFull = false;
    /// The batches' epoch moved to its wrap first.
    bool atWrap = false;
};

/// Fills one chunk for @p root as ChunkFiller::fill reads it, as the file's
/// header says.
void fillChunk(Draw& draw, const Interpreter& interpreter, const CellGeometry cells,
               const NodeIndex root, const NodeIndex flatRoot, const BlockKernel& kernel,
               const Fill& fill, const std::string& label, Tally& tally) {
    ++tally.chunks;
    Interpreter::CornerCache memo(interpreter.cacheSize(), Sharing::Lattice);
    Interpreter::CornerCache reference(interpreter.cacheSize(), Sharing::LastCell);
    const auto note = [&tally, &label](const std::string& what) {
        if (tally.differing++ == 0) {
            tally.first = label + ": " + what;
        }
    };
    const std::int32_t baseX = fill.chunkX * 16;
    const std::int32_t baseZ = fill.chunkZ * 16;
    if (fill.atWrap) {
        memo.advanceColumnEpoch(std::numeric_limits<std::uint32_t>::max() - 2U);
    }
    if (fill.nearlyFull) {
        // A flat_cache's relocated reads, each at a 4x4 column of its own
        // (kept in the memo's hash, which no column count bounds), to some
        // dozens short of the memo's bound: a batch's corners then pass it,
        // and the memo forgets itself while they are kept.
        const std::size_t target = (Interpreter::CornerCache::kMemoCapacity / 2) - 40;
        std::int32_t column = 0;
        while (memo.memoSize() < target) {
            const Point at{
                .x = 100001 + (4 * (column % 1024)), .y = 0, .z = 100001 + (4 * (column / 1024))};
            static_cast<void>(outcomeOf([&] { return interpreter.evaluate(flatRoot, at, memo); }));
            ++column;
            if (column > 200000) {
                break;
            }
        }
    }
    for (std::int32_t i = 0; i < fill.preReads; ++i) {
        const Point at{.x = baseX + draw.below(16),
                       .y = fill.minY + draw.below(fill.topY - fill.minY),
                       .z = baseZ + draw.below(16)};
        const Outcome viaMemo = outcomeOf([&] { return interpreter.evaluate(root, at, memo); });
        const Outcome viaWalk = outcomeOf([&] { return interpreter.evaluate(root, at); });
        ++tally.walked;
        if (!(viaMemo == viaWalk)) {
            note("a read before the fill: memo " + spell(viaMemo) + ", walk " + spell(viaWalk));
        }
    }
    const std::size_t memoBefore = memo.memoSize();
    bool forgot = false;
    const auto finish = [&] {
        tally.batches += memo.columnBatches();
        tally.corners += memo.columnCorners();
        tally.declines += memo.columnDeclines();
        tally.memoForgets += forgot ? 1U : 0U;
        if (memo.columnWidest() == Interpreter::CornerCache::kColumnBatch) {
            ++tally.tallChunks;
        }
    };

    constexpr auto kLanes = static_cast<std::int32_t>(BlockKernel::kLanes);
    BlockKernel::Lanes lanes;
    std::uint64_t declinesBefore = 0;
    for (std::int32_t cellZ = 0; cellZ < 16; cellZ += cells.width) {
        for (std::int32_t cellX = 0; cellX < 16; cellX += cells.width) {
            const std::int32_t endX = std::min(cellX + cells.width, 16);
            const std::int32_t endZ = std::min(cellZ + cells.width, 16);
            const std::int32_t width = endX - cellX;
            const std::int32_t rows = std::max(1, kLanes / width);
            for (std::int32_t bottom = fill.minY; bottom < fill.topY; bottom += cells.height) {
                for (std::int32_t y = bottom; y < std::min(bottom + cells.height, fill.topY); ++y) {
                    for (std::int32_t z = cellZ; z < endZ; z += rows) {
                        const BlockKernel::Blocks blocks{
                            .first = Point{.x = baseX + cellX, .y = y, .z = baseZ + z},
                            .width = width,
                            .depth = std::min(rows, endZ - z),
                            .lanes = draw.percent(10) ? (draw.bits() | 1U) : ~0U};
                        declinesBefore = memo.columnDeclines();
                        const bool ran = interpreter.evaluate(kernel, blocks, lanes, memo);
                        for (std::int32_t row = 0; row < blocks.depth; ++row) {
                            for (std::int32_t column = 0; column < width; ++column) {
                                const auto lane = (static_cast<std::size_t>(row) *
                                                   static_cast<std::size_t>(width)) +
                                                  static_cast<std::size_t>(column);
                                if ((blocks.lanes >> lane & 1U) == 0U) {
                                    continue;
                                }
                                const Point at{.x = blocks.first.x + column,
                                               .y = y,
                                               .z = blocks.first.z + row};
                                ++tally.blocks;
                                const Outcome got =
                                    ran ? Outcome{.refused = false,
                                                  .bits = std::bit_cast<std::uint64_t>(
                                                      lanes.value(lane)),
                                                  .message = {}}
                                        : outcomeOf([&] {
                                              return interpreter.evaluate(kernel, at, memo);
                                          });
                                const Outcome want = outcomeOf(
                                    [&] { return interpreter.evaluate(kernel, at, reference); });
                                const std::string where = "(" + std::to_string(at.x) + ", " +
                                                          std::to_string(at.y) + ", " +
                                                          std::to_string(at.z) + ")";
                                if (!(got == want)) {
                                    note(where + ": batched " + spell(got) + ", last-cell " +
                                         spell(want));
                                }
                                if (draw.percent(3)) {
                                    ++tally.walked;
                                    const Outcome walked =
                                        outcomeOf([&] { return interpreter.evaluate(root, at); });
                                    if (!(walked == want)) {
                                        note(where + ": walk " + spell(walked) + ", last-cell " +
                                             spell(want));
                                    }
                                }
                                if (memo.memoSize() < memoBefore) {
                                    forgot = true;
                                }
                                if (got.refused || want.refused) {
                                    // A fill stops at its first refusal.
                                    ++tally.refusedChunks;
                                    if (memo.columnDeclines() > 0 &&
                                        memo.columnDeclines() == declinesBefore) {
                                        ++tally.refusedAfterDecline;
                                    }
                                    finish();
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    finish();
}

/// Writes @p roots random roots into a fresh pack over @p cells and fills
/// chunks with each.
void fuzzPack(Draw& draw, const CellGeometry cells, const std::int32_t roots, const bool tall,
              Tally& tally) {
    const FuzzTree tree;
    tree.defineNoise("column_a", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.defineNoise("column_b", R"({"firstOctave":-3,"amplitudes":[1.0,0.5]})");
    tree.defineNoise("column_c", R"({"firstOctave":-6,"amplitudes":[1.0,0.0,2.0,1.0]})");
    Writer writer(draw);
    for (std::int32_t i = 0; i < 3; ++i) {
        const std::string name = "column/corners" + std::to_string(i);
        tree.define(name, writer.corner(1));
        writer.shareCorners(name);
    }
    for (std::int32_t i = 0; i < 2; ++i) {
        const std::string name = "column/interpolated" + std::to_string(i);
        tree.define(name, writer.interpolated(0));
        writer.shareInterpolated(name);
    }
    // A flat_cache over a 3D noise, whose relocated reads the memo keeps in
    // its hash, one per 4x4 column: what brings a memo near its bound
    // (Fill::nearlyFull).
    tree.define("column/flat", R"({"type":"minecraft:flat_cache","argument":{
        "type":"minecraft:noise","noise":"column_b","xz_scale":1.0,"y_scale":1.0}})");
    for (std::int32_t i = 0; i < roots; ++i) {
        tree.define("column/root" + std::to_string(i), writer.root());
    }

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises =
        NoiseRegistry::create(pack, graph.referencedNoises(), draw.below(1000000),
                              stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, cells);
    const NodeIndex flatRoot = graph.rootOf(ResourceLocation::parse("minecraft:column/flat"));

    for (std::int32_t i = 0; i < roots; ++i) {
        ++tally.roots;
        const std::string name = "column/root" + std::to_string(i);
        const NodeIndex root = graph.rootOf(ResourceLocation::parse("minecraft:" + name));
        const std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
        if (!kernel.has_value()) {
            ++tally.withoutKernel;
            continue;
        }
        // A near chunk, one past every far noise's reach, one read first
        // through the walk, one whose memo is nearly full or whose batches
        // start at their epoch's wrap; and, in a tall pack, one taller than
        // a batch holds.
        std::vector<Fill> fills{
            Fill{.chunkX = draw.below(5) - 2, .chunkZ = draw.below(5) - 2, .minY = -16, .topY = 40},
            Fill{.chunkX = 400, .chunkZ = -3, .minY = -8, .topY = 24},
            Fill{.chunkX = draw.below(9) - 4,
                 .chunkZ = draw.below(9) - 4,
                 .minY = -16,
                 .topY = 40,
                 .preReads = 6},
            Fill{.chunkX = draw.below(9) - 4,
                 .chunkZ = draw.below(9) - 4,
                 .minY = -8,
                 .topY = 32,
                 .nearlyFull = i == 0,
                 .atWrap = i != 0}};
        if (tall) {
            fills.push_back(Fill{.chunkX = 1, .chunkZ = -1, .minY = -64, .topY = 104});
        }
        for (const Fill& fill : fills) {
            fillChunk(draw, interpreter, cells, root, flatRoot, *kernel, fill,
                      name + " chunk (" + std::to_string(fill.chunkX) + ", " +
                          std::to_string(fill.chunkZ) + ")",
                      tally);
        }
    }
}

void fuzz(std::int64_t seed, std::int32_t packs, Tally& tally) {
    Draw draw(seed);
    static constexpr std::array<CellGeometry, 5> kCells{
        CellGeometry{.width = 4, .height = 8}, CellGeometry{.width = 4, .height = 4},
        CellGeometry{.width = 8, .height = 4}, CellGeometry{.width = 3, .height = 5},
        CellGeometry{.width = 4, .height = 2}};
    for (std::int32_t pack = 0; pack < packs; ++pack) {
        const CellGeometry cells = kCells[static_cast<std::size_t>(pack) % kCells.size()];
        fuzzPack(draw, cells, 3, cells.height == 2, tally);
    }
}

void report(const Tally& tally) {
    CAPTURE(tally.roots, tally.withoutKernel, tally.chunks, tally.blocks, tally.walked,
            tally.refusedChunks, tally.refusedAfterDecline, tally.batches, tally.corners,
            tally.declines, tally.tallChunks, tally.memoForgets, tally.first);
    CHECK(tally.differing == 0U);
    // Batches ran, of many corners each, and some were abandoned: the
    // chunks' first refusals among them, at blocks after the batch that
    // declined.
    CHECK(tally.batches > 100U);
    CHECK(tally.corners > tally.batches * 4U);
    CHECK(tally.declines > 0U);
    CHECK(tally.refusedChunks > 0U);
    CHECK(tally.refusedAfterDecline > 0U);
    CHECK(tally.tallChunks > 0U);
    CHECK(tally.memoForgets > 0U);
    CHECK(tally.roots - tally.withoutKernel > tally.roots / 2U);
}

} // namespace

TEST_CASE("column batches of corners match the walk over random graphs",
          "[density][interpreter][kernel][column-fuzz]") {
    Tally tally;
    fuzz(20261030, 5, tally);
    report(tally);
}

TEST_CASE("column batches of corners match the walk over many random graphs (long)",
          "[.column-fuzz-long]") {
    Tally tally;
    fuzz(5150, 300, tally);
    report(tally);
}

TEST_CASE("a column batch walks an argument as deep as a graph may be",
          "[density][interpreter][kernel][column-fuzz]") {
    // 253 `abs` around a 3D noise under one `interpolated`: 255 deep, a level
    // short of the bound (density::kMaxDepth), every level but the top a node
    // the batch walks lane by lane. Across two files, each under the JSON
    // bound, as density_interpreter_test's kMaxDepth case writes it:
    // `deep_a` sorts first and is resolved first, so `deep_b` meets it
    // memoised.
    const auto nested = [](std::size_t levels, const std::string& innermost) {
        std::string text;
        for (std::size_t i = 0; i < levels; ++i) {
            text += R"({"type":"minecraft:abs","argument":)";
        }
        return text + innermost + std::string(levels, '}');
    };
    const FuzzTree tree;
    tree.defineNoise("column_a", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.define("column/deep_a",
                nested(126, R"({"type":"minecraft:noise","noise":"column_a","xz_scale":1.0,)"
                            R"("y_scale":1.0})"));
    tree.define("column/deep_b", nested(127, R"("minecraft:column/deep_a")"));
    tree.define("column/deep_root",
                R"({"type":"minecraft:interpolated","argument":"minecraft:column/deep_b"})");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 42,
                                                       stratum::density::RandomSource::Xoroshiro);
    const CellGeometry cells{.width = 4, .height = 8};
    const Interpreter interpreter(graph, noises, cells);
    const NodeIndex root = graph.rootOf(ResourceLocation::parse("minecraft:column/deep_root"));
    const std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
    REQUIRE(kernel.has_value());
    Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
    std::size_t differing = 0;
    for (std::int32_t y = -16; y < 48; ++y) {
        for (std::int32_t x = 0; x < 8; x += 3) {
            const Point at{.x = x, .y = y, .z = 5};
            differing += std::bit_cast<std::uint64_t>(interpreter.evaluate(*kernel, at, cache)) !=
                                 std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at))
                             ? 1U
                             : 0U;
        }
    }
    CHECK(differing == 0U);
    CHECK(cache.columnBatches() > 0U);
    CHECK(cache.columnDeclines() == 0U);
}
