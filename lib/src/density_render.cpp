// Stratum — rendering a density function's field to a picture.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/image/png.hpp>
#include <stratum/render/density_render.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::render {

namespace {

/// A ceiling on what will be allocated for one render. Not a policy about
/// image sizes so much as a refusal to turn a mistyped `--size` into an
/// out-of-memory kill with no explanation.
constexpr std::size_t kMaxPixels = std::size_t{64} * 1024 * 1024;
constexpr std::size_t kMaxAxis = 16384U;

/// Samples that are not finite get this rather than a shade, because there
/// is no shade that would be honest. Deliberately unlike anything either
/// ramp produces.
constexpr std::array<std::uint8_t, 3> kNonFinite{255U, 0U, 255U};

/// Neither ramp reaches pure black, so an image that is genuinely uniform
/// still reads as a picture rather than as a failure to draw.
constexpr double kGreyFloor = 20.0;
constexpr double kGreyRange = 235.0;

/// The signed ramp's three anchors: zero, and each end of the scale.
constexpr std::array<double, 3> kNeutral{60.0, 60.0, 60.0};
constexpr std::array<double, 3> kNegative{40.0, 110.0, 255.0};
constexpr std::array<double, 3> kPositive{255.0, 150.0, 40.0};

[[nodiscard]] std::uint8_t toChannel(double value) noexcept {
    // Clamped before the cast: a cast from a double outside the destination's
    // range is undefined, and the arithmetic above can land a hair past 255.
    if (!(value > 0.0)) {
        return 0U;
    }
    if (value >= 255.0) {
        return 255U;
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::array<std::uint8_t, 3>
mix(const std::array<double, 3>& from, const std::array<double, 3>& to, double part) noexcept {
    return {toChannel(from[0] + (part * (to[0] - from[0]))),
            toChannel(from[1] + (part * (to[1] - from[1]))),
            toChannel(from[2] + (part * (to[2] - from[2])))};
}

/// The largest coordinate the area reaches, in 64 bits so that the check for
/// leaving the 32-bit block space happens before the overflow does.
[[nodiscard]] bool spanFitsInBlockSpace(std::int32_t origin, std::size_t count,
                                        std::int32_t step) noexcept {
    if (count == 0U) {
        return true;
    }
    const auto last = static_cast<std::int64_t>(origin) +
                      (static_cast<std::int64_t>(count - 1U) * static_cast<std::int64_t>(step));
    return last >= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) &&
           last <= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
}

void validate(const DensityRenderOptions& options) {
    if (options.width == 0U || options.height == 0U) {
        throw RenderError("a render needs a width and a height greater than zero");
    }
    if (options.width > kMaxAxis || options.height > kMaxAxis) {
        throw RenderError("requested " + std::to_string(options.width) + "x" +
                          std::to_string(options.height) + " pixels; each axis is limited to " +
                          std::to_string(kMaxAxis));
    }
    if (options.width * options.height > kMaxPixels) {
        throw RenderError("requested " + std::to_string(options.width * options.height) +
                          " pixels, more than the " + std::to_string(kMaxPixels) + " allowed");
    }
    if (options.step == 0) {
        throw RenderError("a step of zero would sample one column into every pixel");
    }
    if (!spanFitsInBlockSpace(options.originX, options.width, options.step) ||
        !spanFitsInBlockSpace(options.originZ, options.height, options.step)) {
        throw RenderError("the requested area runs off the 32-bit block coordinate space");
    }
}

} // namespace

bool parseRamp(std::string_view name, Ramp& ramp) noexcept {
    if (name == "grey" || name == "gray") {
        ramp = Ramp::Grey;
        return true;
    }
    if (name == "signed") {
        ramp = Ramp::Signed;
        return true;
    }
    return false;
}

std::string_view rampName(Ramp ramp) noexcept {
    switch (ramp) {
        case Ramp::Grey:
            return "grey";
        case Ramp::Signed:
            return "signed";
    }
    return "unknown";
}

DensityField renderDensity(const density::Interpreter& interpreter, density::NodeIndex root,
                           const DensityRenderOptions& options) {
    validate(options);

    // Before sampling a quarter of a million points, not on the first one:
    // the refusal names a node type, and a caller should hear it immediately
    // rather than after a pause that looks like work.
    interpreter.requireEvaluable(root);

    DensityField field;
    field.values.assign(options.width * options.height, 0.0);
    std::vector<char> finite(options.width * options.height, 0);

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    std::size_t nonFinite = 0;

    for (std::size_t row = 0; row < options.height; ++row) {
        // +x to the right and +z downward, matching the region renderer and
        // every Minecraft map anyone has ever looked at.
        const auto blockZ = static_cast<std::int32_t>(
            static_cast<std::int64_t>(options.originZ) +
            (static_cast<std::int64_t>(row) * static_cast<std::int64_t>(options.step)));

        for (std::size_t column = 0; column < options.width; ++column) {
            const auto blockX = static_cast<std::int32_t>(
                static_cast<std::int64_t>(options.originX) +
                (static_cast<std::int64_t>(column) * static_cast<std::int64_t>(options.step)));

            const double value = interpreter.evaluate(
                root, density::Point{.x = blockX, .y = options.y, .z = blockZ});
            const std::size_t index = (row * options.width) + column;
            field.values[index] = value;

            if (std::isfinite(value)) {
                finite[index] = 1;
                minimum = value < minimum ? value : minimum;
                maximum = value > maximum ? value : maximum;
            } else {
                // Kept out of the range: a single infinity would flatten
                // every real value in the picture to one shade.
                ++nonFinite;
            }
        }
    }

    field.samples = options.width * options.height;
    field.nonFinite = nonFinite;
    field.minimum = nonFinite == field.samples ? 0.0 : minimum;
    field.maximum = nonFinite == field.samples ? 0.0 : maximum;
    field.image = image::Image(options.width, options.height);

    const double span = field.maximum - field.minimum;
    const double magnitude = std::max(std::abs(field.minimum), std::abs(field.maximum));

    for (std::size_t row = 0; row < options.height; ++row) {
        for (std::size_t column = 0; column < options.width; ++column) {
            const std::size_t index = (row * options.width) + column;
            if (finite[index] == 0) {
                field.image.setPixel(column, row, kNonFinite[0], kNonFinite[1], kNonFinite[2]);
                continue;
            }

            const double value = field.values[index];
            std::array<std::uint8_t, 3> colour{};
            switch (options.ramp) {
                case Ramp::Grey: {
                    // A field with no spread is drawn mid-ramp rather than
                    // divided by zero — a constant function is a legitimate
                    // thing to render.
                    const double part = span > 0.0 ? (value - field.minimum) / span : 0.5;
                    const std::uint8_t shade = toChannel(kGreyFloor + (part * kGreyRange));
                    colour = {shade, shade, shade};
                    break;
                }
                case Ramp::Signed: {
                    const double part = magnitude > 0.0 ? value / magnitude : 0.0;
                    colour = part < 0.0 ? mix(kNeutral, kNegative, -part)
                                        : mix(kNeutral, kPositive, part);
                    break;
                }
            }
            field.image.setPixel(column, row, colour[0], colour[1], colour[2]);
        }
    }

    return field;
}

} // namespace stratum::render
