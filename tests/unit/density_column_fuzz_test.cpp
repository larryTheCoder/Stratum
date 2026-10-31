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
// itself; one moves the batches' epoch to its wrap once its first batch has
// run, so that the batches after it cross the wrap with rows stamped before
// it.
//
// Deterministic (a fixed seed through the project's own Xoroshiro) and a few
// seconds long; "[.column-fuzz-long]" runs many times as many packs. Beside
// it, cases written out whole for what the draws seldom reach: an argument
// as deep as a graph may be, a column-invariant node a cell could be filled
// beneath, and NaN and zeros of both signs at a `min`, `max`, `abs` or
// `clamp` whose lanes differ. The draws do not hold those last ones: they
// seldom put an `abs` over lanes that are negative at all, so even an `abs`
// lane that gave its input back would pass them, and a NaN or a zero of
// either sign seldom reaches any of the four where its lanes differ. Nor a
// shifted noise whose lanes part on one axis alone: the draws' parting
// shifts part them on x (and seldom keep z), so a batch that compared only
// one axis before sampling the lanes as one column would pass them too. Nor
// a flat_cache or a spline a cell could be filled beneath over a node the
// batch reads beside it — the draws' shared corners are never under both —
// nor old_blended_noise at a batch's upper lanes alone, nor a batch of 64
// corners whose last lane takes a range_choice's second branch: each has a
// case of its own below.

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
    /// Chunks whose batches crossed their epoch's wrap after a batch had run
    /// (Fill::atWrap).
    std::size_t epochWraps = 0;
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
    /// The batches' epoch moved to the edge of its wrap once the first batch
    /// has run: an advance before any batch is undone by the rows' sizing at
    /// the first, which starts the epoch again from 0.
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
    // Every batch, kept or declined, takes the next epoch, so these two
    // counters together are the epochs taken.
    const auto epochs = [&memo] { return memo.columnBatches() + memo.columnDeclines(); };
    bool advanced = false;
    std::uint64_t epochsAtAdvance = 0;
    const auto finish = [&] {
        tally.batches += memo.columnBatches();
        tally.corners += memo.columnCorners();
        tally.declines += memo.columnDeclines();
        tally.memoForgets += forgot ? 1U : 0U;
        // From the epoch's last value but one, the second epoch taken after
        // it is the wrap's. That is read from the counters, which is sound
        // while a cache's rows are sized once, at its first batch, which the
        // advance waits for; what the wrap must do there is held exactly by
        // density_column_runs_test's case of a row stamped before it.
        if (advanced && epochs() >= epochsAtAdvance + 2U) {
            ++tally.epochWraps;
        }
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
                        if (fill.atWrap && !advanced && epochs() > 0U) {
                            memo.advanceColumnEpoch(std::numeric_limits<std::uint32_t>::max() - 1U);
                            advanced = true;
                            epochsAtAdvance = epochs();
                        }
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
        // cross their epoch's wrap after the first; and, in a tall pack, one
        // taller than a batch holds.
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
            tally.declines, tally.tallChunks, tally.memoForgets, tally.epochWraps, tally.first);
    CHECK(tally.differing == 0U);
    // Batches ran, of many corners each, and some were abandoned: the
    // chunks' first refusals among them, at blocks after the batch that
    // declined. Batches crossed their epoch's wrap.
    CHECK(tally.batches > 100U);
    CHECK(tally.corners > tally.batches * 4U);
    CHECK(tally.declines > 0U);
    CHECK(tally.refusedChunks > 0U);
    CHECK(tally.refusedAfterDecline > 0U);
    CHECK(tally.tallChunks > 0U);
    CHECK(tally.memoForgets > 0U);
    CHECK(tally.epochWraps > 0U);
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

TEST_CASE("a batch reads no column-invariant node beneath which a cell could be filled",
          "[density][interpreter][kernel][column-fuzz]") {
    // A batch reads a column-invariant node once, through the walk, at its
    // first lane's point (Interpreter::evaluateLanes); a node of which
    // something beneath fills a cell or scans points — `interpolated`,
    // `cache_all_in_cell`, `find_top_surface` — is not one it takes
    // (Op::laneWalk), invariant or not, so the walk never opens a batch inside
    // one. A `find_top_surface` over a density and bounds that are all
    // column-invariant is itself invariant; an argument reaching it is
    // evaluated a corner at a time, and an argument reaching only an
    // invariant noise in its place is batched. Both give the walk's bits.
    const FuzzTree tree;
    tree.defineNoise("invariant_a", R"({"firstOctave":-5,"amplitudes":[1.0,1.0]})");
    const std::string varying =
        R"({"type":"minecraft:noise","noise":"minecraft:invariant_a","xz_scale":1.0,)"
        R"("y_scale":1.0})";
    const std::string flat =
        R"({"type":"minecraft:noise","noise":"minecraft:invariant_a","xz_scale":0.5,)"
        R"("y_scale":0.0})";
    tree.define("column/scan", R"({"type":"minecraft:find_top_surface","density":)" + flat +
                                   R"(,"upper_bound":64.0,"lower_bound":-64,"cell_height":8})");
    tree.define("column/scans",
                R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:add",)"
                R"("argument1":)" +
                    varying + R"(,"argument2":"minecraft:column/scan"}})");
    tree.define("column/flat",
                R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:add",)"
                R"("argument1":)" +
                    varying + R"(,"argument2":)" + flat + "}}");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 9,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 8});
    const auto read = [&](std::string_view name, std::size_t& batches) {
        const NodeIndex root =
            graph.rootOf(ResourceLocation::parse("minecraft:column/" + std::string(name)));
        Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
        std::size_t differing = 0;
        for (std::int32_t y = -32; y < 32; y += 3) {
            for (std::int32_t x = 0; x < 8; x += 3) {
                const Point at{.x = x, .y = y, .z = -6};
                differing += std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at, cache)) !=
                                     std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at))
                                 ? 1U
                                 : 0U;
            }
        }
        batches = cache.columnBatches();
        return differing;
    };
    // The scan is column-invariant, and a batch would otherwise read it once.
    REQUIRE(interpreter.isColumnInvariant(
        graph.rootOf(ResourceLocation::parse("minecraft:column/scan"))));
    std::size_t scanning = 0;
    std::size_t flatBatches = 0;
    CHECK(read("scans", scanning) == 0U);
    CHECK(read("flat", flatBatches) == 0U);
    CHECK(scanning == 0U);
    CHECK(flatBatches > 0U);
}

TEST_CASE("a batch reads no flat_cache or spline beneath which a cell could be filled",
          "[density][interpreter][kernel][column-fuzz]") {
    // A batch reads a flat_cache once, through the walk, at its column's
    // corner, and a spline lane by lane through the walk (evaluateLanes): so
    // neither may be one of its nodes where something beneath fills a cell
    // (Op::laneWalk), as for a column-invariant node above. Here the cell is
    // filled by an `interpolated` over `n`, a 3D noise — one node, named —
    // and the argument reads `n` beside it: were the argument batched, the
    // walk would fill that cell inside the batch, and its batch of `n` would
    // overwrite `n`'s row while the outer batch held it. So the argument's
    // corners are evaluated one at a time, each filling cells of `n`'s
    // lattice in batches of their own, and every value is the walk's.
    const FuzzTree tree;
    tree.defineNoise("beneath_a", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
    tree.define("column/n",
                R"({"type":"minecraft:noise","noise":"minecraft:beneath_a","xz_scale":1.0,)"
                R"("y_scale":1.0})");
    const std::string inner =
        R"({"type":"minecraft:interpolated","argument":"minecraft:column/n"})";
    tree.define("column/flat_fills",
                R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:add",)"
                R"("argument1":"minecraft:column/n","argument2":{"type":"minecraft:flat_cache",)"
                R"("argument":)" +
                    inner + "}}}");
    tree.define("column/spline_fills",
                R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:add",)"
                R"("argument1":"minecraft:column/n","argument2":{"type":"minecraft:spline",)"
                R"("spline":{"coordinate":)" +
                    inner +
                    R"(,"points":[{"location":-0.5,"value":-1.0,"derivative":2.0},)"
                    R"({"location":0.5,"value":1.0,"derivative":0.5}]}}}})");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 13,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 8});
    const NodeIndex n = graph.rootOf(ResourceLocation::parse("minecraft:column/n"));
    using stratum::density::NodeType;
    for (const std::string_view name : {"flat_fills", "spline_fills"}) {
        CAPTURE(name);
        const NodeIndex root =
            graph.rootOf(ResourceLocation::parse("minecraft:column/" + std::string(name)));
        // The shape: the argument adds `n` to a flat_cache or spline over an
        // `interpolated` of the same `n`.
        REQUIRE(graph.node(root).type == NodeType::Interpolated);
        const NodeIndex sum = graph.node(root).arguments[0];
        REQUIRE(graph.node(sum).arguments[0] == n);
        const stratum::density::Node& reader = graph.node(graph.node(sum).arguments[1]);
        const NodeIndex filling = reader.type == NodeType::FlatCache
                                      ? reader.arguments[0]
                                      : graph.spline(reader.spline.value()).coordinate;
        REQUIRE(graph.node(filling).type == NodeType::Interpolated);
        REQUIRE(graph.node(filling).arguments[0] == n);

        Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
        std::size_t differing = 0;
        const auto read = [&](const Point at) {
            differing += std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at, cache)) !=
                                 std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at))
                             ? 1U
                             : 0U;
        };
        // Cell (0, 0, 0). Its corners, each alone, fill `n`'s cells from
        // (0, 0, 0), (0, 0, 4), (4, 0, 0) and (4, 0, 4) in turn — the
        // flat_cache's reads at its column corners, and the spline's at the
        // corners themselves, whose upper four are cells of columns already
        // held. In `n`'s lattice those cells make 4, 2, 2 and 1 batches of 8:
        // nine, and none of the argument's own lattice, the second.
        read(Point{.x = 1, .y = 1, .z = 1});
        CHECK(cache.memoLattices() == 2U);
        CHECK(cache.columnBatches() == 9U);
        CHECK(cache.columnCorners() == 72U);
        for (std::int32_t x = 1; x < 16; x += 4) {
            for (std::int32_t y = 1; y < 60; y += 7) {
                read(Point{.x = x, .y = y, .z = 1});
                read(Point{.x = x + 2, .y = y + 3, .z = 6});
            }
        }
        CHECK(differing == 0U);
        CHECK(cache.columnDeclines() == 0U);
    }
}

TEST_CASE("a batch samples old_blended_noise at its upper lanes alone, each in its own place",
          "[density][interpreter][kernel][column-fuzz]") {
    // A batch samples a noise only at the lanes that need it, gathered to
    // the front of a column and put back lane by lane (blendedLanes). The
    // draws' blended noises are almost always needed at every lane; here a
    // range_choice takes it only at a batch's upper lanes, where `upper` is
    // at least 0.5 (y 32 and above), and a 3D noise below.
    const FuzzTree tree;
    tree.defineNoise("upper_a", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
    tree.define("column/upper", R"({"type":"minecraft:y_clamped_gradient","from_y":0,"to_y":64,)"
                                R"("from_value":0.0,"to_value":1.0})");
    tree.define(
        "column/upper_blended",
        R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:range_choice",)"
        R"("input":"minecraft:column/upper","min_inclusive":0.5,"max_exclusive":1e300,)"
        R"("when_in_range":{"type":"minecraft:old_blended_noise","xz_scale":0.25,"y_scale":0.125,)"
        R"("xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0},)"
        R"("when_out_of_range":{"type":"minecraft:noise","noise":"minecraft:upper_a",)"
        R"("xz_scale":1.0,"y_scale":1.0}}})");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 17,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 8});
    const NodeIndex upper = graph.rootOf(ResourceLocation::parse("minecraft:column/upper"));
    const NodeIndex root = graph.rootOf(ResourceLocation::parse("minecraft:column/upper_blended"));
    // The shape: a batch from y 0 has lanes at y 0, 8, ..., 56, of which the
    // blended noise takes the upper four alone.
    for (std::int32_t y = 0; y < 64; y += 8) {
        CAPTURE(y);
        CHECK((interpreter.evaluate(upper, Point{.x = 0, .y = y, .z = 0}) >= 0.5) == (y >= 32));
    }
    Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
    std::size_t differing = 0;
    const auto read = [&](const Point at) {
        differing += std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at, cache)) !=
                             std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at))
                         ? 1U
                         : 0U;
    };
    // Cell iy 0: four batches of 8, lanes 0..7 at y 0..56.
    read(Point{.x = 1, .y = 1, .z = 1});
    CHECK(cache.columnBatches() == 4U);
    CHECK(cache.columnCorners() == 32U);
    for (std::int32_t y = 1; y < 64; y += 4) {
        read(Point{.x = 1, .y = y, .z = 1});
        read(Point{.x = 7, .y = y + 2, .z = 2});
    }
    CHECK(differing == 0U);
    CHECK(cache.columnDeclines() == 0U);
}

TEST_CASE("a batch of 64 corners makes its last lane's range_choice branch",
          "[density][interpreter][kernel][column-fuzz]") {
    // A batch's lanes are its bits, 64 of them at most (kColumnBatch), and a
    // node made at some lanes only (a range_choice's branch) is made at those
    // bits. At cells 2 high a lattice column is 257 corners, so a run there
    // reaches 64: here the 64th corner, at y 126, takes the range_choice's
    // second branch, as every lane above y 64 does.
    const FuzzTree tree;
    tree.defineNoise("falling_a", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
    tree.define("column/falling", R"({"type":"minecraft:y_clamped_gradient","from_y":0,)"
                                  R"("to_y":128,"from_value":1.0,"to_value":0.0})");
    tree.define("column/falling_branches",
                R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:range_choice",)"
                R"("input":"minecraft:column/falling","min_inclusive":0.5,"max_exclusive":1e300,)"
                R"("when_in_range":{"type":"minecraft:noise","noise":"minecraft:falling_a",)"
                R"("xz_scale":0.5,"y_scale":2.0},)"
                R"("when_out_of_range":{"type":"minecraft:noise","noise":"minecraft:falling_a",)"
                R"("xz_scale":1.0,"y_scale":1.0}}})");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 19,
                                                       stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 2});
    const NodeIndex falling = graph.rootOf(ResourceLocation::parse("minecraft:column/falling"));
    const NodeIndex root =
        graph.rootOf(ResourceLocation::parse("minecraft:column/falling_branches"));
    CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = 64, .z = 0}) >= 0.5);
    CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = 66, .z = 0}) < 0.5);
    CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = 126, .z = 0}) < 0.5);
    Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
    std::size_t differing = 0;
    const auto read = [&](const Point at) {
        differing += std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at, cache)) !=
                             std::bit_cast<std::uint64_t>(interpreter.evaluate(root, at))
                         ? 1U
                         : 0U;
    };
    // Cell iy 0, anchoring the lattice at (0, 0, 0): four batches of 8. Cell
    // iy 65 (y 130): the lattice's top is 66 now; four batches of 8 more.
    read(Point{.x = 1, .y = 1, .z = 1});
    read(Point{.x = 1, .y = 131, .z = 1});
    REQUIRE(cache.columnBatches() == 8U);
    // Cell (8, 0, 8), in four columns that hold nothing: each runs from iy 0
    // towards the top, 66, and stops at 64 corners, iy 0..63 (y 0..126).
    read(Point{.x = 9, .y = 1, .z = 9});
    CHECK(cache.columnBatches() == 12U);
    CHECK(cache.columnCorners() == 64U + (4U * 64U));
    CHECK(cache.columnWidest() == 64U);
    // Cell iy 62 (y 124) of those columns, whose corners at iy 63 the 64th
    // lanes made: held, so no batch.
    read(Point{.x = 9, .y = 125, .z = 9});
    read(Point{.x = 11, .y = 125, .z = 10});
    CHECK(cache.columnBatches() == 12U);
    CHECK(differing == 0U);
    CHECK(cache.columnDeclines() == 0U);
}

TEST_CASE("a batch's min, max, abs and clamp lanes keep the walk's NaN and signed zeros",
          "[density][interpreter][kernel][column-fuzz]") {
    // evaluateNode's `min` and `max` are `first < second ? first : second`
    // and `first > second ? first : second` (minOf, maxOf), and a batch's
    // lanes of them (binaryLanes) must be the same: where an argument is NaN,
    // or the two are zeros of opposite signs, the result is the second
    // argument, which std::min and std::max (or the arguments swapped) do
    // not give. Its `abs` and `clamp` are std::abs and std::clamp, and a
    // batch's lanes of them (unaryLanes) must be those too: std::abs clears
    // a negative zero's sign, which `value < 0.0 ? -value : value` does not,
    // and std::clamp gives a NaN back and keeps a zero's sign at a zero
    // bound, where max(low, min(value, high)) gives `low` for the NaN and
    // the bound for the zero. The random graphs above seldom bring any of
    // these to a node whose lanes differ, since a constant argument is
    // column-invariant and read once through the walk, and their `abs`
    // seldom sees a negative varying lane at all; these bring every one, as
    // the first argument and as the second, against a constant and against
    // lanes that differ down the column:
    //  - NaN at some corners and +0 at the rest: a noise clamped to [0, 1],
    //    taken past the largest double and times zero — at a `min` or `max`,
    //    under `abs`, and under `clamp` with bounds on both sides of zero and
    //    with both above it;
    //  - zeros of both signs, a noise times 0.0 and times -0.0, made visible
    //    as -1 and 1 through `invert` and `clamp`: at a `min` or `max`
    //    against each other and against a constant zero, under `abs` (every
    //    value then 1), and under `clamp` at a lower bound of zero and at an
    //    upper one.
    // Each is the argument of an `interpolated` read through a memo cache —
    // block by block and in a kernel's runs — whose batches take it whole
    // and never decline, and every value is the uncached walk's, bit for bit.
    const FuzzTree tree;
    tree.defineNoise("minmax_a", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.defineNoise("minmax_b", R"({"firstOctave":-3,"amplitudes":[1.0,0.5]})");
    const auto pair = [](std::string_view type, const std::string& first,
                         const std::string& second) {
        return R"({"type":"minecraft:)" + std::string(type) + R"(","argument1":)" + first +
               R"(,"argument2":)" + second + "}";
    };
    const auto single = [](std::string_view type, const std::string& argument) {
        return R"({"type":"minecraft:)" + std::string(type) + R"(","argument":)" + argument + "}";
    };
    const auto clamp = [](const std::string& input, std::string_view low, std::string_view high) {
        return R"({"type":"minecraft:clamp","input":)" + input + R"(,"min":)" + std::string(low) +
               R"(,"max":)" + std::string(high) + "}";
    };
    const std::string noise =
        R"({"type":"minecraft:noise","noise":"minecraft:minmax_a","xz_scale":1.0,"y_scale":1.0})";
    const std::string other =
        R"({"type":"minecraft:noise","noise":"minecraft:minmax_b","xz_scale":0.5,"y_scale":2.0})";
    // NaN where the noise is above zero (infinity times zero), +0 elsewhere.
    const std::string nan =
        pair("mul", pair("mul", pair("mul", clamp(noise, "0.0", "1.0"), "1e308"), "1e308"), "0.0");
    // A zero of the noise's sign, one of the other sign, and one of the
    // other noise's other sign.
    const std::string plus = pair("mul", noise, "0.0");
    const std::string minus = pair("mul", noise, "-0.0");
    const std::string otherMinus = pair("mul", other, "-0.0");
    const auto visible = [&](const std::string& zero) {
        return clamp(single("invert", zero), "-1.0", "1.0");
    };
    // What the walk's values must show at an argument, so that each is
    // known to bring the case it is written for.
    enum class Shows : std::uint8_t {
        Bits,        // the walk's bits, and nothing more asked
        SomeNaN,     // NaN at some blocks and not at others
        BothSigns,   // values of both signs
        AllNegative, // every value below zero (-1, its corners all -1)
        AllPositive, // every value above zero (1, its corners all 1)
    };

    struct Argument {
        std::string_view name;
        std::string json;
        Shows shows;
    };

    const std::array<Argument, 24> arguments{{
        // NaN reaches some cells' corners and not others'.
        {.name = "nan", .json = nan, .shows = Shows::SomeNaN},
        {.name = "min_nan_constant", .json = pair("min", nan, "0.5"), .shows = Shows::Bits},
        {.name = "min_constant_nan", .json = pair("min", "0.5", nan), .shows = Shows::Bits},
        {.name = "max_nan_constant", .json = pair("max", nan, "-0.5"), .shows = Shows::Bits},
        {.name = "max_constant_nan", .json = pair("max", "-0.5", nan), .shows = Shows::Bits},
        {.name = "min_nan_noise", .json = pair("min", nan, other), .shows = Shows::Bits},
        {.name = "min_noise_nan", .json = pair("min", other, nan), .shows = Shows::Bits},
        {.name = "max_nan_noise", .json = pair("max", nan, other), .shows = Shows::Bits},
        {.name = "max_noise_nan", .json = pair("max", other, nan), .shows = Shows::Bits},
        // Zeros of both signs reach the `min` or `max`, and its second
        // argument's sign shows through.
        {.name = "min_zeros", .json = visible(pair("min", plus, minus)), .shows = Shows::BothSigns},
        {.name = "min_zeros_swapped",
         .json = visible(pair("min", minus, plus)),
         .shows = Shows::BothSigns},
        {.name = "max_zeros", .json = visible(pair("max", plus, minus)), .shows = Shows::BothSigns},
        {.name = "max_zeros_swapped",
         .json = visible(pair("max", minus, plus)),
         .shows = Shows::BothSigns},
        {.name = "min_zeros_noise",
         .json = visible(pair("min", plus, otherMinus)),
         .shows = Shows::BothSigns},
        {.name = "max_zeros_noise",
         .json = visible(pair("max", otherMinus, plus)),
         .shows = Shows::BothSigns},
        // Zeros tie, so `min` gives its second argument, the constant's
        // -0.0, at every corner: every value -1.
        {.name = "min_zero_constant",
         .json = visible(pair("min", plus, "-0.0")),
         .shows = Shows::AllNegative},
        {.name = "max_zero_constant",
         .json = visible(pair("max", "0.0", minus)),
         .shows = Shows::BothSigns},
        // `abs` keeps the NaN a NaN (and clears its sign: inf * 0's NaN is
        // negative on x86-64), and makes every zero +0, so that every
        // visible value is 1.
        {.name = "abs_nan", .json = single("abs", nan), .shows = Shows::SomeNaN},
        {.name = "abs_zeros", .json = visible(single("abs", plus)), .shows = Shows::AllPositive},
        {.name = "abs_zeros_minus",
         .json = visible(single("abs", minus)),
         .shows = Shows::AllPositive},
        // `clamp` gives the NaN back between bounds on both sides of zero and
        // between bounds above it, and keeps both zeros' signs at a bound of
        // zero, low or high.
        {.name = "clamp_nan", .json = clamp(nan, "-1.0", "1.0"), .shows = Shows::SomeNaN},
        {.name = "clamp_nan_above", .json = clamp(nan, "0.25", "0.75"), .shows = Shows::SomeNaN},
        {.name = "clamp_zeros_low",
         .json = visible(clamp(minus, "0.0", "1.0")),
         .shows = Shows::BothSigns},
        {.name = "clamp_zeros_high",
         .json = visible(clamp(plus, "-1.0", "0.0")),
         .shows = Shows::BothSigns},
    }};
    for (const Argument& argument : arguments) {
        tree.define("lanes/" + std::string(argument.name),
                    R"({"type":"minecraft:interpolated","argument":)" + argument.json + "}");
    }
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 31,
                                                       stratum::density::RandomSource::Xoroshiro);
    const CellGeometry cells{.width = 4, .height = 8};
    const Interpreter interpreter(graph, noises, cells);
    constexpr std::int32_t kBaseX = -16;
    constexpr std::int32_t kBaseZ = 32;
    constexpr std::int32_t kMinY = -24;
    constexpr std::int32_t kTopY = 40;
    for (const Argument& argument : arguments) {
        CAPTURE(argument.name);
        const NodeIndex root =
            graph.rootOf(ResourceLocation::parse("minecraft:lanes/" + std::string(argument.name)));
        const std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
        REQUIRE(kernel.has_value());
        std::size_t values = 0;
        std::size_t differing = 0;
        std::size_t nans = 0;
        std::size_t negatives = 0;
        std::size_t positives = 0;
        const auto hold = [&](const double got, const Point at) {
            const double want = interpreter.evaluate(root, at);
            ++values;
            differing +=
                std::bit_cast<std::uint64_t>(got) != std::bit_cast<std::uint64_t>(want) ? 1U : 0U;
            nans += std::isnan(want) ? 1U : 0U;
            negatives += want < 0.0 ? 1U : 0U;
            positives += want > 0.0 ? 1U : 0U;
        };
        // Block by block, a few columns of every cell of a chunk.
        Interpreter::CornerCache memo(interpreter.cacheSize(), Sharing::Lattice);
        for (std::int32_t z = 0; z < 16; z += 5) {
            for (std::int32_t x = 0; x < 16; x += 3) {
                for (std::int32_t y = kMinY; y < kTopY; ++y) {
                    const Point at{.x = kBaseX + x, .y = y, .z = kBaseZ + z};
                    hold(interpreter.evaluate(root, at, memo), at);
                }
            }
        }
        // A kernel's runs, a cell's row of blocks at a time, over four cells
        // of the chunk, as ChunkFiller::fill reads them.
        Interpreter::CornerCache runs(interpreter.cacheSize(), Sharing::Lattice);
        BlockKernel::Lanes lanes;
        for (std::int32_t cellZ = 0; cellZ < 8; cellZ += cells.width) {
            for (std::int32_t cellX = 0; cellX < 8; cellX += cells.width) {
                for (std::int32_t y = kMinY; y < kTopY; ++y) {
                    const BlockKernel::Blocks blocks{
                        .first = Point{.x = kBaseX + cellX, .y = y, .z = kBaseZ + cellZ},
                        .width = cells.width,
                        .depth = cells.width,
                        .lanes = ~0U};
                    REQUIRE(interpreter.evaluate(*kernel, blocks, lanes, runs));
                    for (std::int32_t row = 0; row < blocks.depth; ++row) {
                        for (std::int32_t column = 0; column < blocks.width; ++column) {
                            const auto lane = (static_cast<std::size_t>(row) *
                                               static_cast<std::size_t>(blocks.width)) +
                                              static_cast<std::size_t>(column);
                            hold(lanes.value(lane), Point{.x = blocks.first.x + column,
                                                          .y = y,
                                                          .z = blocks.first.z + row});
                        }
                    }
                }
            }
        }
        CAPTURE(values, nans, negatives, positives, memo.columnBatches(), runs.columnBatches());
        CHECK(differing == 0U);
        CHECK(memo.columnBatches() > 0U);
        CHECK(memo.columnDeclines() == 0U);
        CHECK(runs.columnBatches() > 0U);
        CHECK(runs.columnDeclines() == 0U);
        switch (argument.shows) {
            case Shows::Bits:
                break;
            case Shows::SomeNaN:
                CHECK(nans > 0U);
                CHECK(nans < values);
                break;
            case Shows::BothSigns:
                CHECK(negatives > 0U);
                CHECK(positives > 0U);
                break;
            case Shows::AllNegative:
                CHECK(negatives == values);
                break;
            case Shows::AllPositive:
                CHECK(positives == values);
                break;
        }
    }
}

TEST_CASE("a batch samples a shifted noise as one column only where every lane's x and z are "
          "the first's",
          "[density][interpreter][kernel][column-fuzz]") {
    // shiftedLanes samples a batch's lanes down one column where every lane's
    // shifted x and shifted z are the first lane's, bit for bit, and each lane
    // alone otherwise. Vanilla's shifts are column-invariant, and the random
    // graphs above part the lanes on x, so neither comparison is held alone
    // there; these part them on z alone, on x alone, on both, on neither (the
    // y shift alone varying, so the one column), and at some lanes only: a
    // shift that is zero at most corners and not at the rest. Each shift
    // varies down a lattice column (checked at the corners a batch takes), so
    // a batch that compared one axis less would sample a parted lane at the
    // first lane's x or z. Each is the argument of an `interpolated` read
    // through a memo cache, block by block and in a kernel's runs, and every
    // value is the uncached walk's, bit for bit.
    const FuzzTree tree;
    tree.defineNoise("shifted_sampled", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
    tree.defineNoise("shifted_by_a", R"({"firstOctave":-3,"amplitudes":[1.0]})");
    tree.defineNoise("shifted_by_b", R"({"firstOctave":-5,"amplitudes":[1.0,1.0]})");
    const std::string byA =
        R"({"type":"minecraft:mul","argument1":{"type":"minecraft:noise","noise":"minecraft:shifted_by_a","xz_scale":1.0,"y_scale":1.0},"argument2":8.0})";
    const std::string byB =
        R"({"type":"minecraft:mul","argument1":{"type":"minecraft:noise","noise":"minecraft:shifted_by_b","xz_scale":0.5,"y_scale":2.0},"argument2":8.0})";
    // Zero where the noise is below 0.3, and the noise's own shift above it.
    const std::string someLanes =
        R"({"type":"minecraft:max","argument1":0.0,"argument2":{"type":"minecraft:add","argument1":)" +
        byA + R"(,"argument2":-2.4}})";
    const auto shifted = [](const std::string& shiftX, const std::string& shiftY,
                            const std::string& shiftZ) {
        return R"({"type":"minecraft:shifted_noise","noise":"minecraft:shifted_sampled","xz_scale":1.0,"y_scale":0.5,"shift_x":)" +
               shiftX + R"(,"shift_y":)" + shiftY + R"(,"shift_z":)" + shiftZ + "}";
    };

    struct Argument {
        std::string_view name;
        std::string shiftX;
        std::string shiftY;
        std::string shiftZ;
        // The shifts that must vary down a lattice column (x, then z).
        bool xParts;
        bool zParts;
    };

    const std::array<Argument, 6> arguments{{
        {.name = "z_only",
         .shiftX = "0.0",
         .shiftY = "0.0",
         .shiftZ = byA,
         .xParts = false,
         .zParts = true},
        {.name = "x_only",
         .shiftX = byA,
         .shiftY = "0.0",
         .shiftZ = "0.0",
         .xParts = true,
         .zParts = false},
        {.name = "both",
         .shiftX = byA,
         .shiftY = "0.0",
         .shiftZ = byB,
         .xParts = true,
         .zParts = true},
        {.name = "neither",
         .shiftX = "0.0",
         .shiftY = byB,
         .shiftZ = "0.0",
         .xParts = false,
         .zParts = false},
        {.name = "z_some_lanes",
         .shiftX = "0.0",
         .shiftY = "0.0",
         .shiftZ = someLanes,
         .xParts = false,
         .zParts = true},
        {.name = "x_some_lanes",
         .shiftX = someLanes,
         .shiftY = "0.0",
         .shiftZ = "0.0",
         .xParts = true,
         .zParts = false},
    }};
    for (const Argument& argument : arguments) {
        tree.define("lanes/" + std::string(argument.name),
                    R"({"type":"minecraft:interpolated","argument":)" +
                        shifted(argument.shiftX, argument.shiftY, argument.shiftZ) + "}");
        tree.define("shifts/" + std::string(argument.name) + "_x", argument.shiftX);
        tree.define("shifts/" + std::string(argument.name) + "_z", argument.shiftZ);
    }
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 31,
                                                       stratum::density::RandomSource::Xoroshiro);
    const CellGeometry cells{.width = 4, .height = 8};
    const Interpreter interpreter(graph, noises, cells);
    constexpr std::int32_t kBaseX = -16;
    constexpr std::int32_t kBaseZ = 32;
    constexpr std::int32_t kMinY = -24;
    constexpr std::int32_t kTopY = 40;
    const auto rootOf = [&graph](const std::string& name) {
        return graph.rootOf(ResourceLocation::parse("minecraft:" + name));
    };
    for (const Argument& argument : arguments) {
        CAPTURE(argument.name);
        // Whether a shift parts the lanes of some lattice column the reads
        // below batch: its value at the column's corners, (x, cell-height
        // multiples of y, z), not one double.
        const auto parts = [&](const NodeIndex shift) {
            std::size_t parted = 0;
            for (std::int32_t z = 0; z <= 16; z += cells.width) {
                for (std::int32_t x = 0; x <= 16; x += cells.width) {
                    const Point bottom{.x = kBaseX + x, .y = kMinY, .z = kBaseZ + z};
                    const double first = interpreter.evaluate(shift, bottom);
                    for (std::int32_t y = kMinY + cells.height; y <= kTopY; y += cells.height) {
                        const double at = interpreter.evaluate(
                            shift, Point{.x = bottom.x, .y = y, .z = bottom.z});
                        if (std::bit_cast<std::uint64_t>(at) !=
                            std::bit_cast<std::uint64_t>(first)) {
                            ++parted;
                            break;
                        }
                    }
                }
            }
            return parted;
        };
        const std::size_t xParted = parts(rootOf("shifts/" + std::string(argument.name) + "_x"));
        const std::size_t zParted = parts(rootOf("shifts/" + std::string(argument.name) + "_z"));
        CAPTURE(xParted, zParted);
        CHECK((xParted > 0U) == argument.xParts);
        CHECK((zParted > 0U) == argument.zParts);

        const NodeIndex root = rootOf("lanes/" + std::string(argument.name));
        const std::optional<BlockKernel> kernel = interpreter.blockKernel(root);
        REQUIRE(kernel.has_value());
        std::size_t values = 0;
        std::size_t differing = 0;
        const auto hold = [&](const double got, const Point at) {
            const double want = interpreter.evaluate(root, at);
            ++values;
            differing +=
                std::bit_cast<std::uint64_t>(got) != std::bit_cast<std::uint64_t>(want) ? 1U : 0U;
        };
        // Block by block, a few columns of every cell of a chunk.
        Interpreter::CornerCache memo(interpreter.cacheSize(), Sharing::Lattice);
        for (std::int32_t z = 0; z < 16; z += 5) {
            for (std::int32_t x = 0; x < 16; x += 3) {
                for (std::int32_t y = kMinY; y < kTopY; ++y) {
                    const Point at{.x = kBaseX + x, .y = y, .z = kBaseZ + z};
                    hold(interpreter.evaluate(root, at, memo), at);
                }
            }
        }
        // A kernel's runs, a cell's row of blocks at a time, over four cells
        // of the chunk, as ChunkFiller::fill reads them.
        Interpreter::CornerCache runs(interpreter.cacheSize(), Sharing::Lattice);
        BlockKernel::Lanes lanes;
        for (std::int32_t cellZ = 0; cellZ < 8; cellZ += cells.width) {
            for (std::int32_t cellX = 0; cellX < 8; cellX += cells.width) {
                for (std::int32_t y = kMinY; y < kTopY; ++y) {
                    const BlockKernel::Blocks blocks{
                        .first = Point{.x = kBaseX + cellX, .y = y, .z = kBaseZ + cellZ},
                        .width = cells.width,
                        .depth = cells.width,
                        .lanes = ~0U};
                    REQUIRE(interpreter.evaluate(*kernel, blocks, lanes, runs));
                    for (std::int32_t row = 0; row < blocks.depth; ++row) {
                        for (std::int32_t column = 0; column < blocks.width; ++column) {
                            const auto lane = (static_cast<std::size_t>(row) *
                                               static_cast<std::size_t>(blocks.width)) +
                                              static_cast<std::size_t>(column);
                            hold(lanes.value(lane), Point{.x = blocks.first.x + column,
                                                          .y = y,
                                                          .z = blocks.first.z + row});
                        }
                    }
                }
            }
        }
        CAPTURE(values, memo.columnBatches(), runs.columnBatches());
        CHECK(differing == 0U);
        CHECK(memo.columnBatches() > 0U);
        CHECK(memo.columnDeclines() == 0U);
        CHECK(runs.columnBatches() > 0U);
        CHECK(runs.columnDeclines() == 0U);
    }
}
