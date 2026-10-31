// Stratum — how far a column run reaches and which batches stand, counted exactly.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Under the memo, a corner a lattice holds and does not hold yet is evaluated
// with the corners above it up its lattice column, in one batch
// (Interpreter::batchedCorner): as far as the highest corner any cell of that
// lattice has asked for, or kColumnGrow (8) corners from the corner while
// that is lower, and never past a corner already held, past the lattice's top
// layer, past a corner whose y leaves int32, or past kColumnBatch (64)
// corners (MemoTable::columnRun). A run of one corner is no batch: the corner
// is evaluated alone. A batch that cannot stand for every corner it took —
// a lane its evaluation would refuse — is declined, keeping nothing, and the
// lattice evaluates every corner alone from then on (declineColumns).
//
// None of that changes a value: a corner evaluated early, again, or alone is
// the same double. So the column fuzz, which holds values, cannot see a run
// that reached one corner too few or too many, a top a cell partly outside
// the lattice raised too little or too far, a corner of such a cell taken for
// the hash's though the lattice holds it, a batch made of one corner or a run
// of two evaluated alone, a batch declined over a branch none of its lanes
// takes, or a lattice that kept trying batches after one declined. These
// cases read a few hand-placed cells at vanilla's 4x8 cells, where a lattice
// is 5 x 65 x 5 corners anchored at its first cell's lower corner, and hold
// the cache's FOR TESTS counters (columnBatches, columnCorners, columnWidest,
// columnDeclines) and the memo's size to the counts the rules above give,
// each worked out in a comment, beside every value held to the uncached walk
// bit for bit. One case moves the batches' epoch to its wrap once a batch has
// run, where a row stamped before the wrap must not read as made after it.

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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

/// A worldgen tree written on the fly, removed with the test.
class RunsTree {
public:
    RunsTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-column-runs")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    RunsTree(const RunsTree&) = delete;
    RunsTree& operator=(const RunsTree&) = delete;

    ~RunsTree() {
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

[[nodiscard]] std::string number(double value) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    return {buffer.data(), static_cast<std::size_t>(written)};
}

/// A 3D noise, one of the scales a batch samples down a column.
constexpr std::string_view kNoise =
    R"({"type":"minecraft:noise","noise":"minecraft:runs","xz_scale":1.0,"y_scale":1.0})";

/// The lattices' cells: vanilla's, 4 wide and 8 high.
constexpr CellGeometry kCells{.width = 4, .height = 8};

/// The pack every case reads, each root an `interpolated` over an argument a
/// batch walks:
///
///   * `plain`, a 3D noise, and `other`, the same noise at other scales (a
///     node of its own, so a row of its own);
///   * `tall`, a noise that refuses wherever y passes about 36 blocks — its
///     lattice along y leaves int32 there (NormalNoise::accepts) — and
///     `declines`, over it;
///   * `branches`, a range_choice taking the 3D noise where `upper` (a
///     gradient from 0 at y 0 to 1 at y 64) is at least 0.5, so at y 32 and
///     above, and `tall` below: `tall` is only ever evaluated below y 32;
///   * `branches_within`, its mirror, a range_choice taking `tall` where
///     `falling` (a gradient from 1 at y 0 to 0 at y 64) is at least 0.5, so
///     at y 32 and below, and the 3D noise above: `tall` is its first branch,
///     and only ever evaluated at y 32 and below;
///   * `nested`, a range_choice taking, where `falling` is at least 0.5 (y 32
///     and below), an inner range_choice, which takes the 3D noise where
///     `falling` is at least 0.7 (y 16 and below) and `tall` elsewhere: so
///     `tall` at y 24 and 32 alone, and the 3D noise above y 32;
///   * `cache_once`, `cache_2d` and `blend_density` over the 3D noise.
class Runs {
public:
    Runs() {
        tree_.defineNoise("runs", R"({"firstOctave":-4,"amplitudes":[1.0,0.5]})");
        tree_.defineNoise("runs_tall", R"({"firstOctave":-4,"amplitudes":[1.0]})");
        tree_.define("runs/plain",
                     R"({"type":"minecraft:interpolated","argument":)" + std::string(kNoise) + "}");
        tree_.define(
            "runs/other",
            R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:noise","noise":"minecraft:runs","xz_scale":0.5,"y_scale":2.0}})");
        const double tallScale = 2147483648.0 * 16.0 / (1.0181268882175227 * 36.0);
        tree_.define(
            "runs/tall",
            R"({"type":"minecraft:noise","noise":"minecraft:runs_tall","xz_scale":1.0,"y_scale":)" +
                number(tallScale) + "}");
        tree_.define("runs/declines",
                     R"({"type":"minecraft:interpolated","argument":"minecraft:runs/tall"})");
        tree_.define(
            "runs/upper",
            R"({"type":"minecraft:y_clamped_gradient","from_y":0,"to_y":64,"from_value":0.0,"to_value":1.0})");
        tree_.define(
            "runs/branches",
            R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:range_choice","input":"minecraft:runs/upper","min_inclusive":0.5,"max_exclusive":1e300,"when_in_range":)" +
                std::string(kNoise) + R"(,"when_out_of_range":"minecraft:runs/tall"}})");
        tree_.define(
            "runs/falling",
            R"({"type":"minecraft:y_clamped_gradient","from_y":0,"to_y":64,"from_value":1.0,"to_value":0.0})");
        tree_.define(
            "runs/branches_within",
            R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:range_choice","input":"minecraft:runs/falling","min_inclusive":0.5,"max_exclusive":1e300,"when_in_range":"minecraft:runs/tall","when_out_of_range":)" +
                std::string(kNoise) + "}}");
        tree_.define(
            "runs/nested",
            R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:range_choice","input":"minecraft:runs/falling","min_inclusive":0.5,"max_exclusive":1e300,"when_in_range":{"type":"minecraft:range_choice","input":"minecraft:runs/falling","min_inclusive":0.7,"max_exclusive":1e300,"when_in_range":)" +
                std::string(kNoise) +
                R"(,"when_out_of_range":"minecraft:runs/tall"},"when_out_of_range":)" +
                std::string(kNoise) + "}}");
        for (const std::string_view type : kPassThrough) {
            tree_.define("runs/" + std::string(type),
                         R"({"type":"minecraft:interpolated","argument":{"type":"minecraft:)" +
                             std::string(type) + R"(","argument":)" + std::string(kNoise) + "}}");
        }
        pack_.emplace(tree_.pack());
        graph_.emplace(Graph::resolveAll(*pack_));
        noises_.emplace(NoiseRegistry::create(*pack_, graph_->referencedNoises(), 20261031,
                                              stratum::density::RandomSource::Xoroshiro));
        interpreter_.emplace(*graph_, *noises_, kCells);
    }

    /// The types evaluateNode reads as their argument's value, which a batch
    /// passes through to the argument's lanes.
    static constexpr std::array<std::string_view, 3> kPassThrough{"cache_once", "cache_2d",
                                                                  "blend_density"};

    [[nodiscard]] NodeIndex root(std::string_view name) const {
        return graph_->rootOf(ResourceLocation::parse("minecraft:runs/" + std::string(name)));
    }

    [[nodiscard]] const Interpreter& interpreter() const { return *interpreter_; }

    /// Whether @p root through @p cache at @p at gives the uncached walk's
    /// bits.
    [[nodiscard]] bool walks(NodeIndex root, Point at, Interpreter::CornerCache& cache) const {
        return std::bit_cast<std::uint64_t>(interpreter_->evaluate(root, at, cache)) ==
               std::bit_cast<std::uint64_t>(interpreter_->evaluate(root, at));
    }

    [[nodiscard]] Interpreter::CornerCache cache() const {
        return {interpreter_->cacheSize(), Sharing::Lattice};
    }

private:
    RunsTree tree_;
    std::optional<Pack> pack_;
    std::optional<Graph> graph_;
    std::optional<NoiseRegistry> noises_;
    std::optional<Interpreter> interpreter_;
};

} // namespace

// Every count below starts from a cell's corners in their order (x outermost,
// then y, then z): a cell from lower corner (x, y, z) whole inside a lattice
// asks for its four lattice columns (x, z), (x, z + 4), (x + 4, z) and
// (x + 4, z + 4), at its iy and iy + 1 each. The first corner asked for in a
// column runs up it; the one above it is then held. So a cell whose columns
// all hold nothing makes four batches, and its asking raises the lattice's
// top to iy + 1. A cell partly outside the lattice is taken corner by corner:
// each corner inside raises the top to its own iy, so the highest of them,
// and each corner outside is the hash's, kept by no batch.

TEST_CASE("a column run stops below a corner already held",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0, anchoring the lattice at (0, 0, 0): top 1, so each column
    // runs the kColumnGrow corners iy 0..7. Four batches of 8.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(memo.columnBatches() == 4U);
    CHECK(memo.columnCorners() == 32U);
    // Cell iy 12 (y 96): top 13, each column runs iy 12..19 (12 + 7 is past
    // the top). Four more batches of 8.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 97, .z = 1}, memo));
    CHECK(memo.columnBatches() == 8U);
    CHECK(memo.columnCorners() == 64U);
    // Cell iy 8 (y 64): each column would run iy 8..15, but iy 12 is held, so
    // iy 8..11. Four batches of 4, not of 8: 80 corners, not 96.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 65, .z = 1}, memo));
    CHECK(memo.columnBatches() == 12U);
    CHECK(memo.columnCorners() == 80U);
    CHECK(memo.columnWidest() == 8U);
    CHECK(memo.columnDeclines() == 0U);
    // Every corner the batches made is kept once, and nothing else is kept.
    CHECK(memo.memoSize() == 80U);
    // The cells between, read through what those runs kept.
    for (std::int32_t y = 1; y < 160; y += 9) {
        CHECK(runs.walks(plain, Point{.x = 2, .y = y, .z = 3}, memo));
    }
}

TEST_CASE("a column run stops below a corner whose y leaves int32",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // The cell from y 2147483624, anchoring the lattice there: its corners up
    // a column are at 2147483624, ...632 and ...640, and the next would be
    // 2^31, past int32. Each of its four columns runs 3 corners, not the 8 it
    // grows to elsewhere.
    const Point high{.x = 1, .y = std::numeric_limits<std::int32_t>::max() - 20, .z = 1};
    CHECK(runs.walks(plain, high, memo));
    CHECK(runs.walks(plain, Point{.x = 3, .y = high.y + 5, .z = 2}, memo));
    CHECK(memo.columnBatches() == 4U);
    CHECK(memo.columnCorners() == 12U);
    CHECK(memo.columnWidest() == 3U);
    CHECK(memo.columnDeclines() == 0U);
}

TEST_CASE("a column run of one corner is evaluated alone, not as a batch",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0: four batches of 8 (iy 0..7). Cell iy 9 (y 72): top 10, and
    // each column runs iy 9..16, four batches of 8.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(runs.walks(plain, Point{.x = 1, .y = 73, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 8U);
    REQUIRE(memo.memoSize() == 64U);
    // Cell iy 8 (y 64): in each column iy 8 is not held and iy 9 is, so its
    // run is the one corner, evaluated alone. Four corners kept, by no batch.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 65, .z = 1}, memo));
    CHECK(memo.memoSize() == 68U);
    CHECK(memo.columnBatches() == 8U);
    CHECK(memo.columnCorners() == 64U);
    CHECK(memo.columnDeclines() == 0U);
}

TEST_CASE("a column run of two corners is a batch, the smallest there is",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0: four batches of 8 (iy 0..7). Cell iy 10 (y 80): top 11, and
    // each column runs iy 10..17, four batches of 8.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(runs.walks(plain, Point{.x = 1, .y = 81, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 8U);
    REQUIRE(memo.columnCorners() == 64U);
    REQUIRE(memo.memoSize() == 64U);
    // Cell iy 8 (y 64): in each column iy 8 and 9 are not held and iy 10 is,
    // so its run is those two corners — four batches of 2, and the corner
    // above each run's first is then held. Every corner kept is a batch's,
    // 72 of 72. Were a run of two evaluated alone, as one of one is, the
    // cell's 8 corners would be kept by no batch: 8 batches, 64 corners.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 65, .z = 1}, memo));
    CHECK(memo.columnBatches() == 12U);
    CHECK(memo.columnCorners() == 72U);
    CHECK(memo.memoSize() == 72U);
    CHECK(memo.columnWidest() == 8U);
    CHECK(memo.columnDeclines() == 0U);
}

TEST_CASE("a column run reaches the highest corner any cell of its lattice has asked for",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0, then cell iy 25 (y 200) in the same columns: that cell asks
    // for corners up to iy 26, the lattice's top now. Eight batches of 8.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(runs.walks(plain, Point{.x = 1, .y = 201, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 8U);
    REQUIRE(memo.columnWidest() == 8U);
    // Cell (8, 0, 8), in four columns that hold nothing: each runs from iy 0
    // to the top, iy 26 — 27 corners, the widest batch.
    CHECK(runs.walks(plain, Point{.x = 9, .y = 1, .z = 9}, memo));
    CHECK(memo.columnBatches() == 12U);
    CHECK(memo.columnCorners() == 32U + 32U + (4U * 27U));
    CHECK(memo.columnWidest() == 27U);
    // The corners the run made early, which the cells above then read.
    for (std::int32_t y = 9; y < 216; y += 23) {
        CHECK(runs.walks(plain, Point{.x = 10, .y = y, .z = 11}, memo));
    }
    CHECK(memo.columnBatches() == 12U);
}

TEST_CASE("a column run reaches the top a cell partly outside its lattice raised it to",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0, anchoring the lattice at (0, 0, 0): top 1, four batches of 8.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 4U);
    REQUIRE(memo.memoSize() == 32U);
    // Cell (16, 160, 0): ix 4, iy 20. The lattice's corners along x end at
    // ix 4 (x 16), so the cell's corners at x 20 are past it and the cell is
    // taken corner by corner. Its four inside, in columns (16, 0) and (16, 4)
    // at iy 20 and 21, raise the top to 21, the highest of them; each of the
    // two columns runs iy 20..27 (kColumnGrow from 20 is further than the
    // top), two batches of 8. The shape: the four corners at x 20 are kept,
    // by no batch — the hash's, 52 kept and 48 of them batched.
    CHECK(runs.walks(plain, Point{.x = 17, .y = 161, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 6U);
    REQUIRE(memo.columnCorners() == 48U);
    REQUIRE(memo.memoSize() == 52U);
    // Cell (8, 0, 8), whole, in four columns that hold nothing: its own
    // asking (iy + 1 = 1) leaves the top at 21, and each column runs iy 0..21
    // — 22 corners, the widest batch. A top the partial cell had left at 1
    // would run kColumnGrow's 8, one it had raised to 22 would run 23.
    CHECK(runs.walks(plain, Point{.x = 9, .y = 1, .z = 9}, memo));
    CHECK(memo.columnBatches() == 10U);
    CHECK(memo.columnWidest() == 22U);
    CHECK(memo.columnCorners() == 48U + (4U * 22U));
    CHECK(memo.memoSize() == 52U + (4U * 22U));
    // The cells up those columns, to iy 19, read what the runs made early.
    for (std::int32_t y = 5; y < 160; y += 19) {
        CHECK(runs.walks(plain, Point{.x = 10, .y = y, .z = 11}, memo));
    }
    CHECK(memo.columnBatches() == 10U);
}

TEST_CASE("a cell partly before its lattice's anchor reads the corners the lattice holds",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0, anchoring the lattice at (0, 0, 0): four batches of 8, its
    // own corners among the 32 kept.
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 4U);
    REQUIRE(memo.memoSize() == 32U);
    // The cell one before it along x, then y, then z: (-4, 0, 0), (0, -8, 0)
    // and (0, 0, -4). Each is taken corner by corner. Its four corners on the
    // anchor's side, at index 0 along that axis, are the lattice's and held
    // from the first cell, so read; its four others, at -1, are past the
    // lattice, the hash's, made and kept. Four kept a cell, and no batch —
    // eight a cell, were a corner at index 0 taken for one past the lattice.
    std::size_t kept = 32;
    for (const Point at : {Point{.x = -3, .y = 1, .z = 1}, Point{.x = 1, .y = -7, .z = 1},
                           Point{.x = 1, .y = 1, .z = -3}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(runs.walks(plain, at, memo));
        kept += 4;
        CHECK(memo.memoSize() == kept);
    }
    CHECK(memo.columnBatches() == 4U);
    CHECK(memo.columnCorners() == 32U);
}

TEST_CASE("a range_choice's branch is evaluated in a batch only at the lanes that take it",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const Interpreter& interpreter = runs.interpreter();
    // The shape: `tall` refuses at the batch's upper lanes (y 40, 48, 56) and
    // not at its lower ones, and `upper` sends the lower lanes (y 0..24) to
    // it and the upper ones (y 32..56) to the 3D noise.
    const NodeIndex tall = runs.root("tall");
    const NodeIndex upper = runs.root("upper");
    for (const std::int32_t y : {0, 8, 16, 24}) {
        CHECK_NOTHROW(interpreter.evaluate(tall, Point{.x = 0, .y = y, .z = 0}));
        CHECK(interpreter.evaluate(upper, Point{.x = 0, .y = y, .z = 0}) < 0.5);
    }
    for (const std::int32_t y : {40, 48, 56}) {
        CHECK_THROWS_AS(interpreter.evaluate(tall, Point{.x = 0, .y = y, .z = 0}), EvalError);
    }
    for (const std::int32_t y : {32, 40, 48, 56}) {
        CHECK(interpreter.evaluate(upper, Point{.x = 0, .y = y, .z = 0}) >= 0.5);
    }
    // Cell iy 0: four batches of 8, each taking `tall` at lanes 0..3 alone,
    // so none declines.
    const NodeIndex branches = runs.root("branches");
    Interpreter::CornerCache memo = runs.cache();
    CHECK(runs.walks(branches, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(memo.columnBatches() == 4U);
    CHECK(memo.columnCorners() == 32U);
    CHECK(memo.columnDeclines() == 0U);
    for (std::int32_t y = 4; y < 60; y += 7) {
        CHECK(runs.walks(branches, Point{.x = 2, .y = y, .z = 3}, memo));
    }
    CHECK(memo.columnDeclines() == 0U);

    // The mirror, `tall` as the first branch: `falling` sends the lower lanes
    // (y 0..32), where `tall` accepts, to it, and the upper ones (y 40..56),
    // where it refuses, to the 3D noise.
    const NodeIndex falling = runs.root("falling");
    for (const std::int32_t y : {0, 8, 16, 24, 32}) {
        CHECK_NOTHROW(interpreter.evaluate(tall, Point{.x = 0, .y = y, .z = 0}));
        CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = y, .z = 0}) >= 0.5);
    }
    for (const std::int32_t y : {40, 48, 56}) {
        CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = y, .z = 0}) < 0.5);
    }
    // Cell iy 0: four batches of 8, each taking `tall` at lanes 0..4 alone,
    // so none declines — where `tall` were made at every lane, the first
    // batch would decline and the lattice make none.
    const NodeIndex within = runs.root("branches_within");
    Interpreter::CornerCache mirrored = runs.cache();
    CHECK(runs.walks(within, Point{.x = 1, .y = 1, .z = 1}, mirrored));
    CHECK(mirrored.columnBatches() == 4U);
    CHECK(mirrored.columnCorners() == 32U);
    CHECK(mirrored.columnDeclines() == 0U);
    for (std::int32_t y = 4; y < 60; y += 7) {
        CHECK(runs.walks(within, Point{.x = 2, .y = y, .z = 3}, mirrored));
    }
    CHECK(mirrored.columnDeclines() == 0U);
}

TEST_CASE("a range_choice in another's branch evaluates its own branches only at the lanes it "
          "was given",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const Interpreter& interpreter = runs.interpreter();
    // The shape: the outer range_choice gives its inner one the lanes where
    // `falling` is at least 0.5 (y 0..32); of those, the inner one sends the
    // ones where it is below 0.7 (y 24 and 32) to `tall`, which accepts
    // there. The lanes the inner one was not given (y 40..56) are below 0.7
    // too, and `tall` refuses at each.
    const NodeIndex tall = runs.root("tall");
    const NodeIndex falling = runs.root("falling");
    for (const std::int32_t y : {0, 8, 16}) {
        CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = y, .z = 0}) >= 0.7);
    }
    for (const std::int32_t y : {24, 32}) {
        const double value = interpreter.evaluate(falling, Point{.x = 0, .y = y, .z = 0});
        CHECK(value >= 0.5);
        CHECK(value < 0.7);
        CHECK_NOTHROW(interpreter.evaluate(tall, Point{.x = 0, .y = y, .z = 0}));
    }
    for (const std::int32_t y : {40, 48, 56}) {
        CHECK(interpreter.evaluate(falling, Point{.x = 0, .y = y, .z = 0}) < 0.5);
        CHECK_THROWS_AS(interpreter.evaluate(tall, Point{.x = 0, .y = y, .z = 0}), EvalError);
    }
    // Cell iy 0: four batches of 8, each taking `tall` at lanes 3 and 4
    // alone, so none declines — where the inner one took its second branch
    // at every lane of the batch outside its first, lanes 5..7 among them,
    // the first batch would decline and the lattice make none.
    const NodeIndex nested = runs.root("nested");
    Interpreter::CornerCache memo = runs.cache();
    CHECK(runs.walks(nested, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(memo.columnBatches() == 4U);
    CHECK(memo.columnCorners() == 32U);
    CHECK(memo.columnDeclines() == 0U);
    for (std::int32_t y = 4; y < 60; y += 7) {
        CHECK(runs.walks(nested, Point{.x = 2, .y = y, .z = 3}, memo));
    }
    CHECK(memo.columnDeclines() == 0U);
}

TEST_CASE("a batch passes cache_once, cache_2d and blend_density through to their argument",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    for (const std::string_view type : Runs::kPassThrough) {
        CAPTURE(type);
        const NodeIndex root = runs.root(type);
        Interpreter::CornerCache memo = runs.cache();
        // Cell iy 0: four batches of 8, the argument's lanes, as for `plain`.
        CHECK(runs.walks(root, Point{.x = 1, .y = 1, .z = 1}, memo));
        CHECK(memo.columnBatches() == 4U);
        CHECK(memo.columnCorners() == 32U);
        CHECK(memo.columnDeclines() == 0U);
        CHECK(runs.walks(root, Point{.x = 3, .y = 13, .z = 2}, memo));
    }
}

TEST_CASE("a lattice whose batch declined evaluates every corner alone from then on",
          "[density][interpreter][kernel][column-runs]") {
    const Runs runs;
    const NodeIndex declines = runs.root("declines");
    Interpreter::CornerCache memo = runs.cache();
    // Cell iy 0: its first corner's run, iy 0..7 (y 0..56), reaches `tall`
    // where it refuses (y 40 and up), so the batch declines and the corner is
    // evaluated alone; the lattice's other seven corners, and every corner of
    // cell (4, 8, 4) after, are evaluated alone too — one decline in all, no
    // batch. The fifteen corners of the two cells (they share one) are kept.
    CHECK(runs.walks(declines, Point{.x = 1, .y = 1, .z = 1}, memo));
    CHECK(runs.walks(declines, Point{.x = 5, .y = 9, .z = 5}, memo));
    CHECK(memo.columnDeclines() == 1U);
    CHECK(memo.columnBatches() == 0U);
    CHECK(memo.columnCorners() == 0U);
    CHECK(memo.memoSize() == 15U);
}

TEST_CASE("a row stamped before the batches' epoch wraps is not made after it",
          "[density][interpreter][kernel][column-runs]") {
    // Each batch takes the next epoch and makes a node's row only where the
    // row's stamp is not that epoch; at the wrap every stamp is cleared. An
    // advance before any batch is undone by the rows' sizing at the first
    // batch, so this one comes after a batch has run.
    const Runs runs;
    const NodeIndex plain = runs.root("plain");
    const NodeIndex other = runs.root("other");
    Interpreter::CornerCache memo = runs.cache();
    // Four batches of `plain`'s noise, epochs 1 to 4: its row is last stamped
    // 4, with the corners of lattice column (4, 4).
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, memo));
    REQUIRE(memo.columnBatches() == 4U);
    REQUIRE(memo.columnDeclines() == 0U);
    memo.advanceColumnEpoch(std::numeric_limits<std::uint32_t>::max() - 1U);
    // Four batches of `other`'s noise: the epoch's last value, then the wrap
    // and 1, 2 and 3. Every batch, kept or declined, takes an epoch, so four
    // past the advance cross the wrap at the second.
    CHECK(runs.walks(other, Point{.x = 1, .y = 1, .z = 1}, memo));
    REQUIRE(memo.columnBatches() + memo.columnDeclines() == 8U);
    // `plain` again, at cell (16, 0, 16): the lattice's far column, one batch
    // of 8 at epoch 4 — the stamp `plain`'s row carried from before the wrap,
    // which must read as not made.
    CHECK(runs.walks(plain, Point{.x = 17, .y = 1, .z = 17}, memo));
    CHECK(runs.walks(plain, Point{.x = 18, .y = 30, .z = 19}, memo));
    CHECK(memo.columnBatches() == 9U);
    CHECK(memo.columnCorners() == 72U);
    // The same with the two swapped, on a cache of its own: the row carried
    // over the wrap is `other`'s noise's, in a slot of its own, not `plain`'s.
    // The wrap clears every row's stamp, whichever slot it is in.
    Interpreter::CornerCache swapped = runs.cache();
    CHECK(runs.walks(other, Point{.x = 1, .y = 1, .z = 1}, swapped));
    REQUIRE(swapped.columnBatches() == 4U);
    REQUIRE(swapped.columnDeclines() == 0U);
    swapped.advanceColumnEpoch(std::numeric_limits<std::uint32_t>::max() - 1U);
    CHECK(runs.walks(plain, Point{.x = 1, .y = 1, .z = 1}, swapped));
    REQUIRE(swapped.columnBatches() + swapped.columnDeclines() == 8U);
    CHECK(runs.walks(other, Point{.x = 17, .y = 1, .z = 17}, swapped));
    CHECK(runs.walks(other, Point{.x = 18, .y = 30, .z = 19}, swapped));
    CHECK(swapped.columnBatches() == 9U);
    CHECK(swapped.columnCorners() == 72U);
}
