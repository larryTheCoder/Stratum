// Stratum — minimal PNG writer.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Just enough PNG to write what `cli render` produces: 8-bit truecolour, no
// interlacing, one IDAT. Written here rather than pulled in as a dependency
// because the format's required parts are small and zlib is already linked.
//
// Encoding is deterministic: the same image always produces the same bytes,
// so a rendered image can be diffed or hashed like any other output.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace stratum::image {

class WriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Row-major 8-bit RGB.
class Image {
public:
    Image() = default;
    Image(std::size_t width, std::size_t height);

    [[nodiscard]] std::size_t width() const noexcept { return width_; }

    [[nodiscard]] std::size_t height() const noexcept { return height_; }

    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

    void setPixel(std::size_t x, std::size_t y, std::uint8_t red, std::uint8_t green,
                  std::uint8_t blue);
    [[nodiscard]] std::array<std::uint8_t, 3> pixel(std::size_t x, std::size_t y) const;

private:
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

[[nodiscard]] std::vector<std::byte> encodePng(const Image& image);

void writePng(const std::filesystem::path& path, const Image& image);

} // namespace stratum::image
