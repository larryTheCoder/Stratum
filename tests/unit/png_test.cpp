// Stratum — PNG writer tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The encoder is checked by decoding what it produced: chunk framing and
// CRCs by hand, pixel data by inflating the IDAT. A "it didn't crash" test
// would not notice a scanline filter byte in the wrong place.

#include <stratum/image/png.hpp>

#include <catch2/catch_test_macros.hpp>

#define ZLIB_CONST
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using stratum::image::encodePng;
using stratum::image::Image;
using stratum::image::WriteError;

[[nodiscard]] std::uint32_t readBigEndian32(const std::vector<std::byte>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<std::uint32_t>(bytes[at + i]);
    }
    return value;
}

[[nodiscard]] std::string chunkType(const std::vector<std::byte>& bytes, std::size_t at) {
    std::string type;
    for (std::size_t i = 0; i < 4; ++i) {
        type += static_cast<char>(bytes[at + i]);
    }
    return type;
}

struct PngChunk {
    std::string type;
    std::vector<std::byte> payload;
};

/// Walks the chunk structure, verifying every CRC on the way.
[[nodiscard]] std::vector<PngChunk> parsePng(const std::vector<std::byte>& bytes) {
    constexpr std::array<std::uint8_t, 8> kSignature{0x89U, 0x50U, 0x4EU, 0x47U,
                                                     0x0DU, 0x0AU, 0x1AU, 0x0AU};
    REQUIRE(bytes.size() > kSignature.size());
    for (std::size_t i = 0; i < kSignature.size(); ++i) {
        REQUIRE(static_cast<std::uint8_t>(bytes[i]) == kSignature[i]);
    }

    std::vector<PngChunk> chunks;
    std::size_t at = kSignature.size();
    while (at + 12U <= bytes.size()) {
        const auto length = static_cast<std::size_t>(readBigEndian32(bytes, at));
        const std::string type = chunkType(bytes, at + 4U);

        const uLong expected =
            crc32(crc32(0L, nullptr, 0), reinterpret_cast<const Bytef*>(bytes.data() + at + 4U),
                  static_cast<uInt>(length + 4U));
        REQUIRE(readBigEndian32(bytes, at + 8U + length) == static_cast<std::uint32_t>(expected));

        chunks.push_back(PngChunk{type,
                                  {bytes.begin() + static_cast<std::ptrdiff_t>(at + 8U),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(at + 8U + length)}});
        at += 12U + length;
    }
    REQUIRE(at == bytes.size());
    return chunks;
}

[[nodiscard]] std::vector<std::byte> inflateAll(const std::vector<std::byte>& input) {
    z_stream stream{};
    REQUIRE(inflateInit(&stream) == Z_OK);
    stream.next_in = reinterpret_cast<const Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());

    std::vector<std::byte> output;
    std::vector<std::byte> buffer(16U * 1024U);
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        REQUIRE((status == Z_OK || status == Z_STREAM_END));
        const std::size_t produced = buffer.size() - static_cast<std::size_t>(stream.avail_out);
        output.insert(output.end(), buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(produced));
    } while (status != Z_STREAM_END);
    inflateEnd(&stream);
    return output;
}

} // namespace

TEST_CASE("an encoded image has the chunks a PNG must have", "[png]") {
    Image image(4, 3);
    const std::vector<std::byte> encoded = encodePng(image);
    const std::vector<PngChunk> chunks = parsePng(encoded);

    REQUIRE(chunks.size() == 3U);
    CHECK(chunks[0].type == "IHDR");
    CHECK(chunks[1].type == "IDAT");
    CHECK(chunks[2].type == "IEND");
    CHECK(chunks[2].payload.empty());

    const std::vector<std::byte>& header = chunks[0].payload;
    REQUIRE(header.size() == 13U);
    CHECK(readBigEndian32(header, 0) == 4U);
    CHECK(readBigEndian32(header, 4) == 3U);
    CHECK(static_cast<std::uint8_t>(header[8]) == 8U);  // 8 bits per channel
    CHECK(static_cast<std::uint8_t>(header[9]) == 2U);  // truecolour
    CHECK(static_cast<std::uint8_t>(header[10]) == 0U); // deflate
    CHECK(static_cast<std::uint8_t>(header[11]) == 0U); // adaptive filtering
    CHECK(static_cast<std::uint8_t>(header[12]) == 0U); // not interlaced
}

TEST_CASE("pixels survive the round trip", "[png]") {
    Image image(3, 2);
    image.setPixel(0, 0, 255, 0, 0);
    image.setPixel(1, 0, 0, 255, 0);
    image.setPixel(2, 0, 0, 0, 255);
    image.setPixel(0, 1, 1, 2, 3);
    image.setPixel(2, 1, 250, 251, 252);

    const std::vector<PngChunk> chunks = parsePng(encodePng(image));
    const std::vector<std::byte> raw = inflateAll(chunks[1].payload);

    // Each scanline is a filter byte followed by width * 3 bytes.
    REQUIRE(raw.size() == 2U * ((3U * 3U) + 1U));
    for (std::size_t y = 0; y < 2; ++y) {
        const std::size_t rowStart = y * ((3U * 3U) + 1U);
        CHECK(static_cast<std::uint8_t>(raw[rowStart]) == 0U); // filter: none
        for (std::size_t x = 0; x < 3; ++x) {
            CAPTURE(x, y);
            const std::array<std::uint8_t, 3> expected = image.pixel(x, y);
            const std::size_t at = rowStart + 1U + (x * 3U);
            CHECK(static_cast<std::uint8_t>(raw[at]) == expected[0]);
            CHECK(static_cast<std::uint8_t>(raw[at + 1U]) == expected[1]);
            CHECK(static_cast<std::uint8_t>(raw[at + 2U]) == expected[2]);
        }
    }
}

TEST_CASE("encoding is deterministic", "[png]") {
    // Rendered output should be diffable and hashable like anything else.
    Image image(16, 16);
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            image.setPixel(x, y, static_cast<std::uint8_t>(x * 16U),
                           static_cast<std::uint8_t>(y * 16U), 128U);
        }
    }
    CHECK(encodePng(image) == encodePng(image));
}

TEST_CASE("the encoder refuses what it cannot encode", "[png][malformed]") {
    CHECK_THROWS_AS(encodePng(Image()), WriteError);

    Image image(2, 2);
    CHECK_THROWS_AS(image.setPixel(2, 0, 0, 0, 0), WriteError);
    CHECK_THROWS_AS(image.setPixel(0, 2, 0, 0, 0), WriteError);
    CHECK_THROWS_AS(image.pixel(5, 5), WriteError);
}

TEST_CASE("a written file matches what the encoder produced", "[png]") {
    Image image(8, 4);
    image.setPixel(3, 2, 10, 20, 30);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "stratum-test-image.png";
    stratum::image::writePng(path, image);

    std::vector<char> written;
    {
        // Scoped so the handle is closed before the file is removed: Windows
        // refuses to delete a file that is still open, where POSIX allows it.
        // Read with an explicit size rather than istreambuf_iterator, which
        // GCC's optimiser cannot prove non-null through and reports as a
        // potential null dereference in <streambuf>.
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream);
        stream.seekg(0, std::ios::end);
        written.resize(static_cast<std::size_t>(stream.tellg()));
        stream.seekg(0, std::ios::beg);
        REQUIRE(stream.read(written.data(), static_cast<std::streamsize>(written.size())));
    }
    std::filesystem::remove(path);

    const std::vector<std::byte> expected = encodePng(image);
    REQUIRE(written.size() == expected.size());
    CHECK(std::memcmp(written.data(), expected.data(), expected.size()) == 0);
}
