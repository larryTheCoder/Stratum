// Stratum — density field rendering tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A picture is a lossy view of the numbers behind it, so almost nothing here
// looks at colours. What matters is that the renderer sampled *where it said
// it would* — an image whose x and z are transposed, or off by one column,
// looks entirely plausible and is entirely wrong — and that it refuses
// rather than clamps when it cannot honour what was asked.
//
// The expected coordinates below are computed in the test, not read back out
// of the renderer, so agreement means the mapping is right rather than
// self-consistent.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/image/png.hpp>
#include <stratum/render/density_render.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using stratum::render::DensityField;
using stratum::render::DensityRenderOptions;
using stratum::render::Ramp;
using stratum::render::RenderError;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const TempTree& define(std::string_view name, std::string_view json) const {
        return write("density_function", name, json);
    }

    const TempTree& defineNoise(std::string_view name, std::string_view json) const {
        return write("noise", name, json);
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    const TempTree& write(std::string_view registry, std::string_view name,
                          std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::ofstream out(file);
        out << json;
        return *this;
    }

    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-render-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

class Pipeline {
public:
    Pipeline(const Pack& pack, std::int64_t seed)
        : graph_(Graph::resolveAll(pack)),
          noises_(NoiseRegistry::create(pack, graph_.referencedNoises(), seed,
                                        stratum::density::RandomSource::Xoroshiro)),
          interpreter_(graph_, noises_) {}

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] DensityField render(std::string_view function,
                                      const DensityRenderOptions& options) const {
        return stratum::render::renderDensity(interpreter_, root(function), options);
    }

    [[nodiscard]] double at(std::string_view function, Point point) const {
        return interpreter_.evaluate(root(function), point);
    }

    [[nodiscard]] stratum::density::NodeIndex root(std::string_view function) const {
        return graph_.rootOf(ResourceLocation::parse(std::string(function)));
    }

private:
    Graph graph_;
    NoiseRegistry noises_;
    Interpreter interpreter_;
};

/// A field that varies in both x and z and is not symmetric between them,
/// which is what makes a transposed render detectable at all: a symmetric
/// field would look identical either way round.
void defineAsymmetricField(const TempTree& tree) {
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0,1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");
}

} // namespace

TEST_CASE("a render samples exactly the columns it claims to", "[render][density]") {
    const TempTree tree;
    defineAsymmetricField(tree);
    const Pipeline pipeline(tree.pack(), 4242);

    // Deliberately not a round origin, not a step of one, and not square:
    // a width-for-height mix-up survives a square image, and an origin of
    // zero hides an origin that is ignored entirely.
    DensityRenderOptions options;
    options.originX = -37;
    options.originZ = 411;
    options.y = 0;
    options.step = 7;
    options.width = 13;
    options.height = 9;

    const DensityField field = pipeline.render("field", options);

    REQUIRE(field.image.width() == options.width);
    REQUIRE(field.image.height() == options.height);
    REQUIRE(field.values.size() == options.width * options.height);
    CHECK(field.samples == options.width * options.height);
    CHECK(field.nonFinite == 0U);

    for (std::size_t row = 0; row < options.height; ++row) {
        for (std::size_t column = 0; column < options.width; ++column) {
            // Rows run along z and columns along x, +z downward, as every
            // Minecraft map is drawn and as the region renderer already does.
            const Point expectedAt{
                .x = options.originX + (static_cast<std::int32_t>(column) * options.step),
                .y = options.y,
                .z = options.originZ + (static_cast<std::int32_t>(row) * options.step)};
            CAPTURE(row, column, expectedAt.x, expectedAt.z);
            CHECK(bits(field.values[(row * options.width) + column]) ==
                  bits(pipeline.at("field", expectedAt)));
        }
    }
}

TEST_CASE("the reported range is the range of the samples", "[render][density]") {
    const TempTree tree;
    defineAsymmetricField(tree);
    const Pipeline pipeline(tree.pack(), 7);

    DensityRenderOptions options;
    options.width = 24;
    options.height = 24;
    options.step = 5;

    const DensityField field = pipeline.render("field", options);

    double lowest = field.values.front();
    double highest = field.values.front();
    for (const double value : field.values) {
        lowest = value < lowest ? value : lowest;
        highest = value > highest ? value : highest;
    }
    CHECK(bits(field.minimum) == bits(lowest));
    CHECK(bits(field.maximum) == bits(highest));
    // A field that came back flat would make every check above vacuous.
    CHECK(field.minimum < field.maximum);
}

TEST_CASE("a constant field renders as one flat colour", "[render][density]") {
    const TempTree tree;
    tree.define("flat", "0.25");
    const Pipeline pipeline(tree.pack(), 0);

    DensityRenderOptions options;
    options.width = 4;
    options.height = 3;
    options.ramp = Ramp::Grey;

    const DensityField field = pipeline.render("flat", options);
    CHECK(bits(field.minimum) == bits(0.25));
    CHECK(bits(field.maximum) == bits(0.25));

    // The grey ramp normalises across the field's own range, and this field
    // has none. Dividing by that zero would give NaN and an undefined cast;
    // the whole image is drawn mid-ramp instead.
    const auto first = field.image.pixel(0, 0);
    for (std::size_t row = 0; row < options.height; ++row) {
        for (std::size_t column = 0; column < options.width; ++column) {
            CAPTURE(row, column);
            CHECK(field.image.pixel(column, row) == first);
        }
    }
    CHECK(first[0] > 0U);
}

TEST_CASE("rendering is byte-for-byte reproducible", "[render][density]") {
    const TempTree tree;
    defineAsymmetricField(tree);
    const Pipeline pipeline(tree.pack(), 31337);

    DensityRenderOptions options;
    options.width = 32;
    options.height = 32;
    options.originX = 64;
    options.originZ = -64;

    // Encoded, not merely compared pixel by pixel: the PNG is the artefact
    // that gets hashed or diffed, so it is the PNG that has to be stable.
    const std::vector<std::byte> first =
        stratum::image::encodePng(pipeline.render("field", options).image);
    const std::vector<std::byte> second =
        stratum::image::encodePng(pipeline.render("field", options).image);
    CHECK(first == second);
    CHECK_FALSE(first.empty());
}

TEST_CASE("a negative step walks the area backwards", "[render][density]") {
    const TempTree tree;
    defineAsymmetricField(tree);
    const Pipeline pipeline(tree.pack(), 11);

    DensityRenderOptions forwards;
    forwards.width = 5;
    forwards.height = 1;
    forwards.step = 4;
    forwards.originX = 0;

    DensityRenderOptions backwards = forwards;
    backwards.step = -4;
    backwards.originX = 16;

    const DensityField left = pipeline.render("field", forwards);
    const DensityField right = pipeline.render("field", backwards);

    // Same five columns, opposite order. Nothing depends on this, but a step
    // that silently became its own absolute value would be a surprise worth
    // catching now rather than in a picture.
    for (std::size_t i = 0; i < forwards.width; ++i) {
        CAPTURE(i);
        CHECK(bits(left.values[i]) == bits(right.values[forwards.width - 1 - i]));
    }
}

TEST_CASE("samples that are not finite are set apart, not folded in", "[render][density]") {
    const TempTree tree;
    // 1/0. Nothing vanilla ships can produce this, but a third-party pack
    // can, and a single infinity folded into the range would flatten every
    // real value in the picture to one shade.
    tree.define("infinite", R"({"type":"minecraft:invert","argument":0.0})");
    const Pipeline pipeline(tree.pack(), 0);

    DensityRenderOptions options;
    options.width = 2;
    options.height = 2;

    const DensityField field = pipeline.render("infinite", options);
    CHECK(field.nonFinite == 4U);
    CHECK(field.samples == 4U);
    // Reported as zero rather than as the infinities that were excluded.
    CHECK(bits(field.minimum) == bits(0.0));
    CHECK(bits(field.maximum) == bits(0.0));

    const auto marker = field.image.pixel(0, 0);
    CHECK(marker[0] == 255U);
    CHECK(marker[1] == 0U);
    CHECK(marker[2] == 255U);
}

TEST_CASE("options that cannot be honoured are refused, not clamped", "[render][density]") {
    const TempTree tree;
    defineAsymmetricField(tree);
    const Pipeline pipeline(tree.pack(), 0);

    const auto render = [&pipeline](const DensityRenderOptions& options) {
        return pipeline.render("field", options);
    };

    DensityRenderOptions zeroWidth;
    zeroWidth.width = 0;
    CHECK_THROWS_AS(render(zeroWidth), RenderError);

    DensityRenderOptions zeroStep;
    zeroStep.step = 0;
    CHECK_THROWS_WITH(render(zeroStep), ContainsSubstring("step of zero"));

    DensityRenderOptions huge;
    huge.width = 100000;
    huge.height = 4;
    CHECK_THROWS_WITH(render(huge), ContainsSubstring("limited to"));

    // The far corner would leave the 32-bit block space. Clamping it would
    // silently render somewhere other than the place that was asked for.
    DensityRenderOptions offTheEdge;
    offTheEdge.originX = 2147483000;
    offTheEdge.width = 4096;
    offTheEdge.step = 16;
    CHECK_THROWS_WITH(render(offTheEdge), ContainsSubstring("block coordinate space"));

    // Right up against the edge is fine: the check is on the last sample,
    // not on a margin past it.
    DensityRenderOptions atTheEdge;
    atTheEdge.originX = 2147483647 - 12;
    atTheEdge.width = 4;
    atTheEdge.height = 1;
    atTheEdge.step = 4;
    CHECK_NOTHROW(render(atTheEdge));
}

TEST_CASE("a function this build cannot evaluate is refused before sampling", "[render][density]") {
    const TempTree tree;
    // end_islands, because it is still refused. This was old_blended_noise
    // until that was settled and started rendering terrain instead.
    tree.define("blended", R"({"type":"minecraft:end_islands"})");
    const Pipeline pipeline(tree.pack(), 0);

    DensityRenderOptions options;
    options.width = 512;
    options.height = 512;

    // Named, and raised from requireEvaluable rather than from the first
    // sample, so a caller hears it immediately instead of after a pause that
    // looks like work.
    CHECK_THROWS_WITH(pipeline.render("blended", options),
                      ContainsSubstring("minecraft:end_islands"));
}

TEST_CASE("ramp names round-trip", "[render][density]") {
    Ramp ramp = Ramp::Signed;
    CHECK(stratum::render::parseRamp("grey", ramp));
    CHECK(ramp == Ramp::Grey);
    // Both spellings, because half the world writes the other one.
    CHECK(stratum::render::parseRamp("gray", ramp));
    CHECK(ramp == Ramp::Grey);
    CHECK(stratum::render::parseRamp("signed", ramp));
    CHECK(ramp == Ramp::Signed);
    CHECK_FALSE(stratum::render::parseRamp("rainbow", ramp));
    CHECK(ramp == Ramp::Signed);

    CHECK(stratum::render::rampName(Ramp::Grey) == "grey");
    CHECK(stratum::render::rampName(Ramp::Signed) == "signed");
}
