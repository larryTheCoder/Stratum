// Stratum — the deepest graphs evaluated on a thread whose stack is 1 MiB.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §8 bounds a density graph at 256 nodes and splines deep
// (density::kMaxDepth) so that the recursions over one fit the stack a
// Windows main thread or a JVM thread gets by default, 1 MiB, in every
// build, the unoptimised ones included, whose frames are the largest. A
// Linux main thread has 8 MiB, so nothing else here would notice a
// recursion that outgrew 1 MiB. This evaluates each of the deepest shapes
// the interpreter's recursions take, a level short of the bound, on a
// thread of exactly 1 MiB:
//
//   * the walk: `abs` 255 deep, read with no cache;
//   * a column batch's lane walk (Interpreter::evaluateLanes) under one
//     `interpolated`, 253 deep below it, through `abs`, through `add` — read
//     through a block kernel's run, as a chunk's fill reads it — and through
//     range_choice's first branch;
//   * `interpolated` nested 254 deep, through a memo cache, a last-cell cache
//     and no cache, each refused at the bottom, where a noise is sampled
//     past its lattice, which is the deepest descent there is and the
//     refusal's message built at its foot.
//
// Where a test can give a thread a stack of its own (POSIX), the stack is
// painted first and the most of it each shape touched is measured; each
// must stay inside a budget of its own, kept well inside the 1 MiB, so that a
// change that thickens a level is caught here long before it overflows a
// thread in the field. "[.stack-report]" prints the figures, which are what
// SPEC §8 records for the `sanitize` preset's build.

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
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#define STRATUM_TEST_PAINTED_STACK 1
#else
#include <thread>
#endif

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

/// The stack each shape runs on: what a Windows main thread or a JVM thread
/// gets by default (SPEC §8).
constexpr std::size_t kThreadStack = std::size_t{1} << 20;

/// What a shape may touch of it, where that is measured: about a sixth more
/// than the most the `sanitize` preset's build (Clang 18, unoptimised, UBSan)
/// took when this was written — some 320 KiB for the walk, 290 for a batch's
/// lane walk, 720 for nested `interpolated` through a memo cache — which is
/// the build whose frames are the largest; the other builds here take far
/// less. The walk and a batch's lane walk are a frame or so a level, nested
/// `interpolated` several (SPEC §8).
constexpr std::size_t kWalkBudget = std::size_t{384} << 10;
constexpr std::size_t kNestedBudget = std::size_t{832} << 10;

/// A worldgen tree written on the fly, removed with the test.
class StackTree {
public:
    StackTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-density-stack")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    StackTree(const StackTree&) = delete;
    StackTree& operator=(const StackTree&) = delete;

    ~StackTree() {
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

/// @p levels of @p open around @p innermost, each closed by a brace.
[[nodiscard]] std::string nested(std::size_t levels, std::string_view open,
                                 std::string_view innermost) {
    std::string text;
    for (std::size_t i = 0; i < levels; ++i) {
        text += open;
    }
    return text + std::string(innermost) + std::string(levels, '}');
}

/// A chain of @p open written as density_interpreter_test's kMaxDepth case
/// writes one, each file under the JSON bound: `<name>/deep_a`, @p inner
/// levels around @p innermost, sorts first and is resolved first, so that
/// `<name>/deep_b`, @p outer levels around a reference to it, meets it
/// memoised.
void chain(const StackTree& tree, std::string_view name, std::string_view open, std::size_t inner,
           std::size_t outer, std::string_view innermost) {
    tree.define("density_function", std::string(name) + "/deep_a", nested(inner, open, innermost));
    tree.define("density_function", std::string(name) + "/deep_b",
                nested(outer, open, "\"minecraft:" + std::string(name) + "/deep_a\""));
}

/// How much of a thread's stack a run touched: 0 where it is not measured.
struct StackUse {
    std::size_t touched = 0;
};

#ifdef STRATUM_TEST_PAINTED_STACK

constexpr unsigned char kPaint = 0xA5;

struct Job {
    const std::function<void()>* work = nullptr;
    std::exception_ptr failure;
};

void* runJob(void* context) {
    Job& job = *static_cast<Job*>(context);
    try {
        (*job.work)();
    } catch (...) {
        job.failure = std::current_exception();
    }
    return nullptr;
}

/// @p work on a thread whose stack is kThreadStack bytes of this test's own,
/// painted first, with a page beneath it that faults: past the stack is a
/// crash, never a write into something else.
StackUse onBoundedStack(const std::function<void()>& work) {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void* const region =
        mmap(nullptr, kThreadStack + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(region != MAP_FAILED);
    REQUIRE(mprotect(region, page, PROT_NONE) == 0);
    auto* const stack = static_cast<unsigned char*>(region) + page;
    std::memset(stack, kPaint, kThreadStack);

    pthread_attr_t attributes;
    REQUIRE(pthread_attr_init(&attributes) == 0);
    REQUIRE(pthread_attr_setstack(&attributes, stack, kThreadStack) == 0);
    Job job;
    job.work = &work;
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, &runJob, &job);
    pthread_attr_destroy(&attributes);
    REQUIRE(created == 0);
    REQUIRE(pthread_join(thread, nullptr) == 0);

    // The stack grows down: the lowest byte that is not the paint is the
    // deepest the thread wrote.
    std::size_t untouched = 0;
    while (untouched < kThreadStack && stack[untouched] == kPaint) {
        ++untouched;
    }
    munmap(region, kThreadStack + page);
    if (job.failure) {
        std::rethrow_exception(job.failure);
    }
    return StackUse{.touched = kThreadStack - untouched};
}

#else

/// @p work on a std::thread: on Windows, where a test cannot hand a thread
/// a stack of its own, it runs on the executable's default reserve, which
/// is 1 MiB unless the link says otherwise (MSVC's /STACK), as this
/// project's does not. Not measured.
StackUse onBoundedStack(const std::function<void()>& work) {
    std::exception_ptr failure;
    std::thread thread([&work, &failure] {
        try {
            work();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    thread.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return StackUse{};
}

#endif

[[nodiscard]] std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

/// The pack every shape reads, and its interpreter over 4x8 cells.
class Shapes {
public:
    Shapes() {
        // A top octave at frequency 16: its lattice leaves int32 a little
        // past 1.3e8 on an axis, which a block reaches (SPEC §8).
        tree_.define("noise", "stack_fast", R"({"firstOctave":4,"amplitudes":[1.0]})");
        const std::string noise =
            R"({"type":"minecraft:noise","noise":"minecraft:stack_fast","xz_scale":1.0,)"
            R"("y_scale":1.0})";
        // 255 `abs` around a constant: 256 deep, the bound.
        chain(tree_, "walk", R"({"type":"minecraft:abs","argument":)", 127, 128, "-0.25");
        // Under one `interpolated`, 253 levels around the noise: 255 deep.
        chain(tree_, "abs", R"({"type":"minecraft:abs","argument":)", 126, 127, noise);
        chain(tree_, "add", R"({"type":"minecraft:add","argument1":0.125,"argument2":)", 126, 127,
              noise);
        chain(tree_, "range",
              R"({"type":"minecraft:range_choice","input":0.0,"min_inclusive":-1.0,)"
              R"("max_exclusive":1.0,"when_out_of_range":1.0,"when_in_range":)",
              126, 127, noise);
        // 254 `interpolated` around the noise: 255 deep.
        chain(tree_, "nested", R"({"type":"minecraft:interpolated","argument":)", 126, 127, noise);
        for (const std::string_view name : {"abs", "add", "range", "nested"}) {
            tree_.define("density_function", std::string(name) + "/root",
                         R"({"type":"minecraft:interpolated","argument":"minecraft:)" +
                             std::string(name) + R"(/deep_b"})");
        }
        pack_.emplace(tree_.pack());
        graph_.emplace(Graph::resolveAll(*pack_));
        noises_.emplace(NoiseRegistry::create(*pack_, graph_->referencedNoises(), 42,
                                              stratum::density::RandomSource::Xoroshiro));
        interpreter_.emplace(*graph_, *noises_, CellGeometry{.width = 4, .height = 8});
    }

    [[nodiscard]] const Interpreter& interpreter() const { return *interpreter_; }

    [[nodiscard]] NodeIndex root(std::string_view name) const {
        return graph_->rootOf(ResourceLocation::parse("minecraft:" + std::string(name)));
    }

private:
    StackTree tree_;
    std::optional<Pack> pack_;
    std::optional<Graph> graph_;
    std::optional<NoiseRegistry> noises_;
    std::optional<Interpreter> interpreter_;
};

/// A block the noise is defined at, and one far past its lattice.
constexpr Point kNear{.x = 6, .y = 19, .z = -3};
constexpr Point kFar{.x = 1500000000, .y = 16, .z = 0};

// Each shape runs on its own thread and says what, if anything, was wrong,
// for the test's own thread to check: Catch2's assertions are made there. A
// value a shape is held to is the walk's, made on the test's own thread
// beforehand (@p walked), so that the shape's thread measures the shape alone.

/// The column batch's lane walk under @p root, read at a block through a
/// memo cache — the batch runs, and declines nothing — and held to the walk.
[[nodiscard]] std::string laneShape(const Shapes& shapes, std::string_view root,
                                    const double walked) {
    const Interpreter& interpreter = shapes.interpreter();
    const NodeIndex index = shapes.root(root);
    Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
    const double value = interpreter.evaluate(index, kNear, cache);
    if (bits(value) != bits(walked)) {
        return "the batch's value is not the walk's";
    }
    if (cache.columnBatches() == 0U || cache.columnDeclines() != 0U) {
        return "no batch ran, or one declined";
    }
    return {};
}

/// The last block of the row of a cell kernelShape runs.
constexpr Point kLastOfRow{.x = 7, .y = kNear.y, .z = -5};

/// The same through a block kernel's run over a row of a cell, as a chunk's
/// fill reads it, its last block held to the walk.
[[nodiscard]] std::string kernelShape(const Shapes& shapes, std::string_view root,
                                      const double walked) {
    const Interpreter& interpreter = shapes.interpreter();
    const NodeIndex index = shapes.root(root);
    const std::optional<BlockKernel> kernel = interpreter.blockKernel(index);
    if (!kernel.has_value()) {
        return "no block kernel";
    }
    Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
    BlockKernel::Lanes lanes;
    const BlockKernel::Blocks blocks{
        .first = Point{.x = 4, .y = kNear.y, .z = -8}, .width = 4, .depth = 4, .lanes = 0xFFFFU};
    if (!interpreter.evaluate(*kernel, blocks, lanes, cache)) {
        return "the kernel's run declined";
    }
    if (bits(lanes.value(15)) != bits(walked)) {
        return "the run's value is not the walk's";
    }
    if (cache.columnBatches() == 0U || cache.columnDeclines() != 0U) {
        return "no batch ran, or one declined";
    }
    return {};
}

/// Nested `interpolated`, read at a block whose cell's every corner reads
/// the next level at a block of its own: the first corner of every level is
/// the block itself, so the descent goes all the way down at once, and is
/// refused at the foot, where the noise is sampled past its lattice.
[[nodiscard]] std::string nestedShape(const Shapes& shapes,
                                      const std::function<double(NodeIndex)>& read) {
    try {
        static_cast<void>(read(shapes.root("nested/root")));
    } catch (const EvalError& error) {
        const std::string refusal = error.what();
        if (refusal.find("samples noise 'minecraft:stack_fast'") == std::string::npos ||
            refusal.find("SPEC §8") == std::string::npos) {
            return "refused otherwise: " + refusal;
        }
        return {};
    }
    return "nothing refused";
}

/// One shape: its name, the budget it is held to, what it touched and what
/// was wrong.
struct Shape {
    std::string name;
    std::size_t budget = 0;
    std::size_t touched = 0;
    std::string problem;
};

/// Every shape on its own bounded thread.
std::vector<Shape> runShapes() {
    const Shapes shapes;
    const Interpreter& interpreter = shapes.interpreter();
    std::vector<Shape> runs;
    const auto run = [&runs](std::string name, std::size_t budget,
                             const std::function<std::string()>& shape) {
        std::string problem;
        const StackUse use = onBoundedStack([&shape, &problem] { problem = shape(); });
        runs.push_back(Shape{.name = std::move(name),
                             .budget = budget,
                             .touched = use.touched,
                             .problem = std::move(problem)});
    };
    run("walk, abs 255 deep, no cache", kWalkBudget, [&]() -> std::string {
        const double value = interpreter.evaluate(shapes.root("walk/deep_b"), kNear);
        return bits(value) == bits(0.25) ? std::string() : std::string("not 0.25");
    });
    const double abs = interpreter.evaluate(shapes.root("abs/root"), kNear);
    run("lane walk, abs 253 deep", kWalkBudget, [&] { return laneShape(shapes, "abs/root", abs); });
    const double add = interpreter.evaluate(shapes.root("add/root"), kLastOfRow);
    run("lane walk, add 253 deep, through a kernel", kWalkBudget,
        [&] { return kernelShape(shapes, "add/root", add); });
    const double range = interpreter.evaluate(shapes.root("range/root"), kNear);
    run("lane walk, range_choice 253 deep", kWalkBudget,
        [&] { return laneShape(shapes, "range/root", range); });
    run("interpolated 254 deep, memo cache", kNestedBudget, [&] {
        Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::Lattice);
        return nestedShape(shapes,
                           [&](NodeIndex root) { return interpreter.evaluate(root, kFar, cache); });
    });
    run("interpolated 254 deep, last-cell cache", kNestedBudget, [&] {
        Interpreter::CornerCache cache(interpreter.cacheSize(), Sharing::LastCell);
        return nestedShape(shapes,
                           [&](NodeIndex root) { return interpreter.evaluate(root, kFar, cache); });
    });
    run("interpolated 254 deep, no cache", kNestedBudget, [&] {
        return nestedShape(shapes,
                           [&](NodeIndex root) { return interpreter.evaluate(root, kFar); });
    });
    return runs;
}

} // namespace

TEST_CASE("the deepest graphs evaluate on a thread whose stack is 1 MiB",
          "[density][interpreter][stack]") {
    const std::vector<Shape> runs = runShapes();
    REQUIRE(runs.size() == 7U);
    for (const Shape& shape : runs) {
        CAPTURE(shape.name, shape.touched, shape.budget);
        CHECK(shape.problem.empty());
#ifdef STRATUM_TEST_PAINTED_STACK
        CHECK(shape.touched > 0U);
#endif
        CHECK(shape.touched <= shape.budget);
    }
}

TEST_CASE("the stack the deepest graphs take, printed", "[.stack-report]") {
    for (const Shape& shape : runShapes()) {
        std::printf("%-48s %8zu bytes (%zu KiB) of a %zu KiB budget%s%s\n", shape.name.c_str(),
                    shape.touched, (shape.touched + 1023U) / 1024U, shape.budget >> 10U,
                    shape.problem.empty() ? "" : ": ", shape.problem.c_str());
    }
}
