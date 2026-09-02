// Stratum — rendering a density function's field to a picture.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §10 puts "heightmap-style rendering" in M2, and the qualifier is
// load-bearing: this is not terrain height. Terrain height needs the cell
// sampler and the block placement that follow it, which is M3. What this
// renders is the value of one density function across an area — continents,
// erosion, offset — shaded the way a heightmap is shaded.
//
// It exists because a number that is wrong by 0.01 everywhere reads exactly
// like a number that is right, and a picture of the same field does not.
// `stratum diff` remains the only thing that establishes parity; this is for
// noticing that a continent moved.
//
// Rendering is deterministic. The colour of a sample depends only on the
// value, the ramp and the field's own observed range — never on a palette
// drawn at random or on iteration order — so the same inputs produce the
// same PNG bytes on every platform, which is what lets a rendered image be
// hashed or diffed like any other output.

#pragma once

#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/image/png.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace stratum::render {

/// Raised for options that cannot be honoured: a zero step, an area that
/// runs off the coordinate space, a size beyond what will be allocated.
/// Refusing is deliberate — a silently clamped area would produce a picture
/// of somewhere other than the place that was asked for.
class RenderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// How a sampled value becomes a colour.
enum class Ramp : std::uint8_t {
    /// Dark to light across the field's own observed range. Shows structure
    /// whatever the values happen to be, at the cost of two renders of
    /// different areas not sharing a scale.
    Grey,
    /// Blue below zero, orange above, grey at zero, scaled by the largest
    /// magnitude present. Most of vanilla's 2D functions are shaped around
    /// the sign of the value — continentalness below zero is ocean — so this
    /// is the default.
    Signed,
};

[[nodiscard]] bool parseRamp(std::string_view name, Ramp& ramp) noexcept;
[[nodiscard]] std::string_view rampName(Ramp ramp) noexcept;

struct DensityRenderOptions {
    /// The block coordinate of the top-left pixel.
    std::int32_t originX = 0;
    std::int32_t originZ = 0;
    /// The height every sample is taken at. Irrelevant for the 2D functions,
    /// which pin it to zero themselves through flat_cache, and not for the
    /// ones that vary with y.
    std::int32_t y = 0;
    /// Blocks per pixel. The default of 4 puts one pixel on each 4x4 column,
    /// which is the resolution vanilla's 2D functions actually vary at —
    /// sampling finer than that magnifies flat_cache's steps rather than
    /// revealing anything.
    std::int32_t step = 4;
    std::size_t width = 512;
    std::size_t height = 512;
    Ramp ramp = Ramp::Signed;
};

/// A rendered field, with the numbers the picture was built from. They are
/// returned rather than left implicit so a caller can say what the shades
/// mean instead of leaving a reader to infer it.
struct DensityField {
    image::Image image;
    /// The samples themselves, row-major and indexed exactly as the image
    /// is: `values[(row * width) + column]`. Returned rather than discarded
    /// because a shade cannot be read back into a number — a caller that
    /// needs the value at a pixel, or a test that needs to check where the
    /// renderer sampled, has nowhere else to get it.
    std::vector<double> values;
    /// Over the finite samples only.
    double minimum = 0.0;
    double maximum = 0.0;
    std::size_t samples = 0;
    /// Samples that came back NaN or infinite. Drawn in their own colour and
    /// counted rather than folded into the range, where one of them would
    /// flatten every other value to a single shade.
    std::size_t nonFinite = 0;
};

/// Samples @p root over the area @p options describes and shades it.
/// Refuses up front — before sampling a quarter of a million points — if the
/// function contains anything this build cannot evaluate, naming the type.
[[nodiscard]] DensityField renderDensity(const density::Interpreter& interpreter,
                                         density::NodeIndex root,
                                         const DensityRenderOptions& options);

} // namespace stratum::render
