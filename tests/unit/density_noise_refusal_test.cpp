// Stratum — a noise sampled past its lattice is refused in the same words by every node that
// samples one. Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Interpreter::sampleNoise refuses, by name, a point a noise is not defined
// at (noise::NormalNoise::accepts), and builds the message out of its way
// (refuseNoiseInput). This pins the whole message — the node's type, the
// noise, the coordinates it was asked for and the block, the limit, every
// digit — for each of the six node types that sample a named noise, through
// every evaluate form, so that how the message is reached cannot change
// what it says.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/noise/perlin.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::CellGeometry;
using stratum::density::EvalError;
using stratum::density::FlatCacheWindow;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using stratum::density::ReadContext;
using Sharing = Interpreter::CornerCache::Sharing;

/// A worldgen tree written on the fly, removed with the test.
class RefusalTree {
public:
    RefusalTree()
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-noise-refusal")) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    RefusalTree(const RefusalTree&) = delete;
    RefusalTree& operator=(const RefusalTree&) = delete;

    ~RefusalTree() {
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

/// The message sampleNoise has always thrown, spelt out.
[[nodiscard]] std::string expected(std::string_view type, double limit, double x, double y,
                                   double z, Point at) {
    std::ostringstream message;
    message << std::setprecision(17) << "'" << type << "' samples noise 'minecraft:fast' at (" << x
            << ", " << y << ", " << z << ") for block (" << at.x << ", " << at.y << ", " << at.z
            << "), past " << limit
            << " on an axis or not a number: there its highest-frequency octave's lattice "
               "cell leaves the 32-bit range, where this build's arithmetic is undefined, "
               "so it refuses rather than guess what the server builds there (SPEC §8)";
    return message.str();
}

} // namespace

TEST_CASE("every node type that samples a named noise refuses a point past its lattice in the "
          "same words",
          "[density][interpreter][noise][refusal]") {
    const RefusalTree tree;
    // A top octave at frequency 16: the lattice leaves int32 a little past
    // 1.3e8 on an axis, which a block coordinate reaches.
    tree.define("noise", "fast", R"({"firstOctave":4,"amplitudes":[1.0]})");
    tree.define("density_function", "noise", R"({"type":"minecraft:noise","noise":"fast",
        "xz_scale":1.0,"y_scale":0.5})");
    tree.define("density_function", "shifted", R"({"type":"minecraft:shifted_noise",
        "noise":"fast","xz_scale":1.0,"y_scale":0.0,"shift_x":0.5,"shift_y":0.0,
        "shift_z":-0.25})");
    tree.define("density_function", "shift", R"({"type":"minecraft:shift","argument":"fast"})");
    tree.define("density_function", "shift_a", R"({"type":"minecraft:shift_a","argument":"fast"})");
    tree.define("density_function", "shift_b", R"({"type":"minecraft:shift_b","argument":"fast"})");
    tree.define("density_function", "weird", R"({"type":"minecraft:weird_scaled_sampler",
        "rarity_value_mapper":"type_1","noise":"fast","input":0.25})");
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 5,
                                                       stratum::density::RandomSource::Xoroshiro);
    const stratum::noise::NormalNoise* fast = noises.find(ResourceLocation::parse("fast"));
    REQUIRE(fast != nullptr);
    const double limit = fast->inputLimit();
    const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 8});

    const Point at{.x = 1500000000, .y = -37, .z = -1200000000};
    const auto x = static_cast<double>(at.x);
    const auto y = static_cast<double>(at.y);
    const auto z = static_cast<double>(at.z);

    struct Case {
        std::string_view root;
        std::string message;
    };

    const std::array<Case, 6> cases{{
        {.root = "noise", .message = expected("minecraft:noise", limit, x, y * 0.5, z, at)},
        {.root = "shifted",
         .message =
             expected("minecraft:shifted_noise", limit, x + 0.5, (y * 0.0) + 0.0, z + -0.25, at)},
        {.root = "shift",
         .message = expected("minecraft:shift", limit, x * 0.25, y * 0.25, z * 0.25, at)},
        {.root = "shift_a",
         .message = expected("minecraft:shift_a", limit, x * 0.25, 0.0, z * 0.25, at)},
        {.root = "shift_b",
         .message = expected("minecraft:shift_b", limit, z * 0.25, x * 0.25, 0.0, at)},
        // type_1 at 0.25 is a rarity of 1.5.
        {.root = "weird",
         .message =
             expected("minecraft:weird_scaled_sampler", limit, x / 1.5, y / 1.5, z / 1.5, at)},
    }};
    const FlatCacheWindow window = FlatCacheWindow::none();
    for (const Case& refused : cases) {
        CAPTURE(refused.root);
        const auto root = graph.rootOf(ResourceLocation::parse(std::string(refused.root)));
        const auto thrown = [&](const auto& evaluate) {
            try {
                static_cast<void>(evaluate());
            } catch (const EvalError& error) {
                return std::string(error.what());
            }
            return std::string("nothing thrown");
        };
        CHECK(thrown([&] { return interpreter.evaluate(root, at); }) == refused.message);
        Interpreter::CornerCache lattice(interpreter.cacheSize());
        CHECK(thrown([&] { return interpreter.evaluate(root, at, lattice); }) == refused.message);
        Interpreter::CornerCache last(interpreter.cacheSize(), Sharing::LastCell);
        CHECK(thrown([&] {
                  return interpreter.evaluate(root, at, last, window, ReadContext::Detached);
              }) == refused.message);
        // And a point the lattice holds is no refusal.
        CHECK_NOTHROW(interpreter.evaluate(root, Point{.x = 1000, .y = -37, .z = -1200}));
    }
}
