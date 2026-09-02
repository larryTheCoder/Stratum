// Stratum — minimal PNG writer.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/image/png.hpp>

#define ZLIB_CONST
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::image {

namespace {

constexpr std::array<std::uint8_t, 8> kSignature{0x89U, 0x50U, 0x4EU, 0x47U,
                                                 0x0DU, 0x0AU, 0x1AU, 0x0AU};

void appendBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

/// Every PNG chunk is length, type, payload, CRC-32 of type+payload.
void appendChunk(std::vector<std::uint8_t>& out, std::string_view type,
                 const std::vector<std::uint8_t>& payload) {
    appendBigEndian32(out, static_cast<std::uint32_t>(payload.size()));

    const std::size_t crcStart = out.size();
    for (const char character : type) {
        out.push_back(static_cast<std::uint8_t>(character));
    }
    out.insert(out.end(), payload.begin(), payload.end());

    const uLong crc = crc32(crc32(0L, nullptr, 0), out.data() + crcStart,
                            static_cast<uInt>(out.size() - crcStart));
    appendBigEndian32(out, static_cast<std::uint32_t>(crc));
}

[[nodiscard]] std::vector<std::uint8_t> deflateBytes(const std::vector<std::uint8_t>& input) {
    uLongf bound = compressBound(static_cast<uLong>(input.size()));
    std::vector<std::uint8_t> output(bound);
    const int status = compress2(output.data(), &bound, input.data(),
                                 static_cast<uLong>(input.size()), Z_DEFAULT_COMPRESSION);
    if (status != Z_OK) {
        throw WriteError("deflate failed while encoding PNG (status " + std::to_string(status) +
                         ")");
    }
    output.resize(bound);
    return output;
}

} // namespace

Image::Image(std::size_t width, std::size_t height)
    : width_(width), height_(height), pixels_(width * height * 3U, 0U) {}

void Image::setPixel(std::size_t x, std::size_t y, std::uint8_t red, std::uint8_t green,
                     std::uint8_t blue) {
    if (x >= width_ || y >= height_) {
        throw WriteError("pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                         ") is outside the image");
    }
    const std::size_t offset = ((y * width_) + x) * 3U;
    pixels_[offset] = red;
    pixels_[offset + 1U] = green;
    pixels_[offset + 2U] = blue;
}

std::array<std::uint8_t, 3> Image::pixel(std::size_t x, std::size_t y) const {
    if (x >= width_ || y >= height_) {
        throw WriteError("pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                         ") is outside the image");
    }
    const std::size_t offset = ((y * width_) + x) * 3U;
    return {pixels_[offset], pixels_[offset + 1U], pixels_[offset + 2U]};
}

std::vector<std::byte> encodePng(const Image& image) {
    if (image.width() == 0 || image.height() == 0) {
        throw WriteError("cannot encode an empty image");
    }

    std::vector<std::uint8_t> out(kSignature.begin(), kSignature.end());

    std::vector<std::uint8_t> header;
    appendBigEndian32(header, static_cast<std::uint32_t>(image.width()));
    appendBigEndian32(header, static_cast<std::uint32_t>(image.height()));
    header.push_back(8U); // bit depth
    header.push_back(2U); // colour type: truecolour
    header.push_back(0U); // compression: deflate
    header.push_back(0U); // filter method
    header.push_back(0U); // no interlacing
    appendChunk(out, "IHDR", header);

    // Every scanline is prefixed with its filter type. Filter 0 (none) keeps
    // the encoder trivially deterministic; these images compress well anyway.
    std::vector<std::uint8_t> raw;
    raw.reserve(image.height() * ((image.width() * 3U) + 1U));
    for (std::size_t y = 0; y < image.height(); ++y) {
        raw.push_back(0U);
        const std::size_t rowStart = y * image.width() * 3U;
        raw.insert(raw.end(), image.pixels().begin() + static_cast<std::ptrdiff_t>(rowStart),
                   image.pixels().begin() +
                       static_cast<std::ptrdiff_t>(rowStart + (image.width() * 3U)));
    }
    appendChunk(out, "IDAT", deflateBytes(raw));
    appendChunk(out, "IEND", {});

    std::vector<std::byte> bytes(out.size());
    std::memcpy(bytes.data(), out.data(), out.size());
    return bytes;
}

void writePng(const std::filesystem::path& path, const Image& image) {
    const std::vector<std::byte> bytes = encodePng(image);
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw WriteError("cannot open for writing: " + path.string());
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw WriteError("short write to: " + path.string());
    }
}

} // namespace stratum::image
