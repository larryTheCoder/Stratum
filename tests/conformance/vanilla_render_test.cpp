// Stratum — rendering vanilla's own density functions.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite renders fields this repository invented. This one renders
// vanilla's, which is where the renderer meets the two things a synthetic
// pack cannot supply: functions deep enough to be slow, and flat_cache,
// whose whole job is to make the value at a block differ from the value at
// the block beside it.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/image/png.hpp>
#include <stratum/render/density_render.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NoiseRegistry;
using stratum::render::DensityField;
using stratum::render::DensityRenderOptions;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::filesystem::path findWorldgenTree() {
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path();
        }
    }
    return {};
}

class Overworld {
public:
    Overworld(const Pack& pack, std::int64_t seed)
        : graph_(Graph::resolveAll(pack)),
          noises_(NoiseRegistry::create(pack, graph_.referencedNoises(), seed)),
          interpreter_(graph_, noises_) {}

    Overworld(const Overworld&) = delete;
    Overworld& operator=(const Overworld&) = delete;

    [[nodiscard]] DensityField render(std::string_view function,
                                      const DensityRenderOptions& options) const {
        return stratum::render::renderDensity(interpreter_, root(function), options);
    }

    [[nodiscard]] stratum::density::NodeIndex root(std::string_view function) const {
        return graph_.rootOf(ResourceLocation::parse(std::string(function)));
    }

private:
    Graph graph_;
    NoiseRegistry noises_;
    Interpreter interpreter_;
};

} // namespace

TEST_CASE("vanilla's 2D functions render into a field with real structure",
          "[conformance][render]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Overworld overworld(Pack::openWorldgenTree(tree), 42);

    DensityRenderOptions options;
    options.width = 96;
    options.height = 96;
    options.step = 4;
    options.originX = -192;
    options.originZ = -192;

    for (const std::string_view function :
         {"overworld/continents", "overworld/erosion", "overworld/ridges", "overworld/offset"}) {
        CAPTURE(function);
        const DensityField field = overworld.render(function, options);

        CHECK(field.samples == options.width * options.height);
        // Nothing in vanilla's 2D chain can produce an infinity, and a
        // magenta pixel in one of these images would mean something in the
        // arithmetic had gone badly wrong rather than merely differently.
        CHECK(field.nonFinite == 0U);
        // Over 384 blocks square, every one of these varies. A field that
        // came back flat would still render, and would still look like a
        // picture.
        CHECK(field.minimum < field.maximum);
        // Loose bounds, deliberately: they are not a parity claim — that is
        // the cubiomes comparison's job — only a guard against a pipeline
        // that has started returning coordinates or garbage.
        CHECK(field.minimum > -10.0);
        CHECK(field.maximum < 10.0);
    }

    // continentalness spans zero over an area this size, which is what makes
    // the signed ramp meaningful: below zero is ocean and above it is land.
    const DensityField continents = overworld.render("overworld/continents", options);
    CHECK(continents.minimum < 0.0);
    CHECK(continents.maximum > 0.0);
}

TEST_CASE("a coarse render agrees with a fine one about the columns they share",
          "[conformance][render]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Overworld overworld(Pack::openWorldgenTree(tree), 1234);

    // Every 2D function vanilla ships is wrapped in flat_cache, so a render
    // stepping one block at a time must produce 4x4 blocks of identical
    // value, and its corners must be exactly what a render stepping four
    // blocks at a time produces. This is the renderer and flat_cache checked
    // against each other end to end: an off-by-one in the coordinate mapping
    // would land the fine render's corners in the wrong cells.
    DensityRenderOptions coarse;
    coarse.width = 24;
    coarse.height = 24;
    coarse.step = 4;
    coarse.originX = 64;
    coarse.originZ = -128;

    DensityRenderOptions fine = coarse;
    fine.step = 1;
    fine.width = coarse.width * 4;
    fine.height = coarse.height * 4;

    const DensityField coarseField = overworld.render("overworld/offset", coarse);
    const DensityField fineField = overworld.render("overworld/offset", fine);

    for (std::size_t row = 0; row < coarse.height; ++row) {
        for (std::size_t column = 0; column < coarse.width; ++column) {
            const double expected = coarseField.values[(row * coarse.width) + column];
            for (std::size_t withinZ = 0; withinZ < 4; ++withinZ) {
                for (std::size_t withinX = 0; withinX < 4; ++withinX) {
                    const std::size_t fineRow = (row * 4) + withinZ;
                    const std::size_t fineColumn = (column * 4) + withinX;
                    CAPTURE(row, column, withinX, withinZ);
                    CHECK(bits(fineField.values[(fineRow * fine.width) + fineColumn]) ==
                          bits(expected));
                }
            }
        }
    }
}

TEST_CASE("rendering vanilla's data is reproducible", "[conformance][render]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);

    DensityRenderOptions options;
    options.width = 64;
    options.height = 64;

    // Rebuilt from the pack each time rather than rendered twice from one
    // pipeline: what has to be stable is the whole path from JSON and a seed
    // to PNG bytes, not just the last step of it.
    const Overworld first(pack, 99);
    const Overworld second(pack, 99);
    const std::vector<std::byte> left =
        stratum::image::encodePng(first.render("overworld/continents", options).image);
    const std::vector<std::byte> right =
        stratum::image::encodePng(second.render("overworld/continents", options).image);

    CHECK(left == right);
    CHECK_FALSE(left.empty());

    // A different seed is a different world; identical bytes would mean the
    // seed never reached the noises.
    const Overworld elsewhere(pack, 100);
    const std::vector<std::byte> other =
        stratum::image::encodePng(elsewhere.render("overworld/continents", options).image);
    CHECK(left != other);
}
