// Stratum — Anvil region container tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Region files are built here rather than fetched: the harness's fixtures are
// Mojang-derived and never committed (SPEC §12), so the container logic is
// exercised against synthetic files covering each framing and each way a file
// can be malformed. What the reader must never do is skip a chunk quietly —
// a conformance diff that silently drops unreadable chunks reports parity it
// did not verify.

#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#define ZLIB_CONST
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using stratum::region::Compression;
using stratum::region::FormatError;
using stratum::region::RegionFile;

constexpr std::size_t kSectorBytes = stratum::region::kSectorBytes;
constexpr std::size_t kHeaderBytes = stratum::region::kHeaderBytes;

[[nodiscard]] std::vector<std::byte> payloadOfLength(std::size_t length, std::uint8_t seed) {
    // Compressible but not uniform, so a real deflate stream comes out.
    std::vector<std::byte> data(length);
    for (std::size_t i = 0; i < length; ++i) {
        data[i] = static_cast<std::byte>((i * 31U + seed) % 251U);
    }
    return data;
}

[[nodiscard]] std::vector<std::byte> deflateBytes(const std::vector<std::byte>& input,
                                                  int windowBits) {
    z_stream stream{};
    REQUIRE(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8,
                         Z_DEFAULT_STRATEGY) == Z_OK);

    std::vector<std::byte> output(deflateBound(&stream, static_cast<uLong>(input.size())) + 32U);
    stream.next_in = reinterpret_cast<const Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int status = deflate(&stream, Z_FINISH);
    const std::size_t produced = output.size() - static_cast<std::size_t>(stream.avail_out);
    deflateEnd(&stream);
    REQUIRE(status == Z_STREAM_END);

    output.resize(produced);
    return output;
}

void writeBigEndian32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    out[at + 1U] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    out[at + 2U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[at + 3U] = static_cast<std::byte>(value & 0xFFU);
}

/// Builds a syntactically valid region file, one framed chunk at a time.
class RegionBuilder {
public:
    RegionBuilder() : bytes_(kHeaderBytes, std::byte{0}) {}

    /// @p framed is the compression byte followed by the payload, exactly as
    /// it appears on disk.
    void addFramed(std::int32_t chunkX, std::int32_t chunkZ, std::uint8_t scheme,
                   const std::vector<std::byte>& payload, std::int32_t timestamp = 1'700'000'000) {
        const std::size_t sectorOffset = bytes_.size() / kSectorBytes;
        const std::uint32_t declared = static_cast<std::uint32_t>(payload.size() + 1U);

        std::vector<std::byte> framed(5U + payload.size());
        writeBigEndian32(framed, 0, declared);
        framed[4] = static_cast<std::byte>(scheme);
        std::memcpy(framed.data() + 5, payload.data(), payload.size());

        const std::size_t sectors = ((framed.size() + kSectorBytes) - 1U) / kSectorBytes;
        framed.resize(sectors * kSectorBytes, std::byte{0});
        bytes_.insert(bytes_.end(), framed.begin(), framed.end());

        const std::size_t index = RegionFile::indexFor(chunkX, chunkZ);
        writeBigEndian32(bytes_, index * 4U,
                         (static_cast<std::uint32_t>(sectorOffset) << 8U) |
                             static_cast<std::uint32_t>(sectors));
        writeBigEndian32(bytes_, kSectorBytes + (index * 4U),
                         static_cast<std::uint32_t>(timestamp));
    }

    void addChunk(std::int32_t chunkX, std::int32_t chunkZ, Compression scheme,
                  const std::vector<std::byte>& raw) {
        switch (scheme) {
            case Compression::Zlib:
                addFramed(chunkX, chunkZ, 2, deflateBytes(raw, 15));
                break;
            case Compression::Gzip:
                addFramed(chunkX, chunkZ, 1, deflateBytes(raw, 15 + 16));
                break;
            case Compression::None:
                addFramed(chunkX, chunkZ, 3, raw);
                break;
            default:
                FAIL("unsupported scheme in the test builder");
        }
    }

    /// Overwrites a sector-table entry, for the malformed-file cases.
    void forceLocation(std::int32_t chunkX, std::int32_t chunkZ, std::uint32_t sectorOffset,
                       std::uint8_t sectorCount) {
        const std::size_t index = RegionFile::indexFor(chunkX, chunkZ);
        writeBigEndian32(bytes_, index * 4U, (sectorOffset << 8U) | sectorCount);
    }

    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return bytes_; }

    [[nodiscard]] RegionFile build() const { return RegionFile::fromBytes(bytes_, "r.0.0.mca"); }

private:
    std::vector<std::byte> bytes_;
};

} // namespace

TEST_CASE("an empty region file is valid and holds nothing", "[region]") {
    // Vanilla creates the file before writing any chunk into it.
    const RegionFile region = RegionFile::fromBytes({}, "r.0.0.mca");
    CHECK(region.chunkCount() == 0U);
    CHECK_FALSE(region.hasChunk(0, 0));
    CHECK(region.timestamp(0, 0) == 0);
    CHECK_THROWS_AS(region.readChunk(0, 0), FormatError);
}

TEST_CASE("chunks round-trip through every supported framing", "[region]") {
    const std::vector<std::byte> small = payloadOfLength(1024, 7);
    // Larger than the reader's inflate buffer, so the streaming loop runs
    // more than once.
    const std::vector<std::byte> large = payloadOfLength(300U * 1024U, 19);

    RegionBuilder builder;
    builder.addChunk(0, 0, Compression::Zlib, small);
    builder.addChunk(1, 0, Compression::Gzip, small);
    builder.addChunk(2, 0, Compression::None, small);
    builder.addChunk(3, 0, Compression::Zlib, large);

    const RegionFile region = builder.build();
    CHECK(region.chunkCount() == 4U);
    CHECK(region.readChunk(0, 0) == small);
    CHECK(region.readChunk(1, 0) == small);
    CHECK(region.readChunk(2, 0) == small);
    CHECK(region.readChunk(3, 0) == large);
}

TEST_CASE("presence, timestamps and absence are reported per chunk", "[region]") {
    RegionBuilder builder;
    builder.addChunk(5, 9, Compression::Zlib, payloadOfLength(64, 3));
    builder.addFramed(31, 31, 3, payloadOfLength(16, 4), 1'234'567);

    const RegionFile region = builder.build();
    CHECK(region.chunkCount() == 2U);
    CHECK(region.hasChunk(5, 9));
    CHECK(region.hasChunk(31, 31));
    CHECK_FALSE(region.hasChunk(6, 9));
    CHECK(region.timestamp(31, 31) == 1'234'567);
    CHECK(region.timestamp(6, 9) == 0);
    CHECK_THROWS_AS(region.readChunk(6, 9), FormatError);
}

TEST_CASE("negative chunk coordinates index the way Java's do", "[region][landmine]") {
    // The whole reason indexFor uses floorMod: with `%`, chunk (-1, -1) would
    // land at index -1 and the world would mirror across the origin.
    CHECK(RegionFile::indexFor(0, 0) == 0U);
    CHECK(RegionFile::indexFor(31, 31) == 1023U);
    CHECK(RegionFile::indexFor(-1, -1) == 1023U);
    CHECK(RegionFile::indexFor(-32, -32) == 0U);
    CHECK(RegionFile::indexFor(-33, -33) == 1023U);
    CHECK(RegionFile::indexFor(32, 64) == 0U);
    CHECK(RegionFile::indexFor(-1, 0) == 31U);
    CHECK(RegionFile::indexFor(0, -1) == 992U);

    // And it must hold end to end, not just in the index helper.
    RegionBuilder builder;
    const std::vector<std::byte> payload = payloadOfLength(128, 11);
    builder.addChunk(-1, -1, Compression::Zlib, payload);
    const RegionFile region = builder.build();
    CHECK(region.hasChunk(-1, -1));
    CHECK(region.hasChunk(31, 31)); // same slot, seen from the other side
    CHECK(region.readChunk(-1, -1) == payload);
}

TEST_CASE("a region file on disk reads the same as one in memory", "[region]") {
    RegionBuilder builder;
    const std::vector<std::byte> payload = payloadOfLength(2048, 23);
    builder.addChunk(2, 3, Compression::Zlib, payload);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "stratum-test-r.0.0.mca";
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out);
        out.write(reinterpret_cast<const char*>(builder.bytes().data()),
                  static_cast<std::streamsize>(builder.bytes().size()));
    }

    const RegionFile region = RegionFile::open(path);
    CHECK(region.name() == "stratum-test-r.0.0.mca");
    CHECK(region.readChunk(2, 3) == payload);
    std::filesystem::remove(path);

    CHECK_THROWS_AS(RegionFile::open(path), FormatError); // now gone
}

TEST_CASE("malformed sector tables are rejected at open, not at read", "[region][malformed]") {
    SECTION("a truncated header") {
        std::vector<std::byte> bytes(100, std::byte{0});
        CHECK_THROWS_AS(RegionFile::fromBytes(bytes, "r.0.0.mca"), FormatError);
    }

    SECTION("an entry pointing into the header") {
        RegionBuilder builder;
        builder.addChunk(0, 0, Compression::None, payloadOfLength(16, 1));
        builder.forceLocation(0, 0, 1, 1);
        CHECK_THROWS_AS(builder.build(), FormatError);
    }

    SECTION("an entry running past the end of the file") {
        RegionBuilder builder;
        builder.addChunk(0, 0, Compression::None, payloadOfLength(16, 1));
        builder.forceLocation(0, 0, 2, 200);
        CHECK_THROWS_AS(builder.build(), FormatError);
    }

    SECTION("a half-empty entry") {
        RegionBuilder builder;
        builder.addChunk(0, 0, Compression::None, payloadOfLength(16, 1));
        builder.forceLocation(0, 0, 3, 0);
        CHECK_THROWS_AS(builder.build(), FormatError);
    }
}

TEST_CASE("inconsistent chunk framing is rejected", "[region][malformed]") {
    SECTION("a payload longer than the sectors reserved for it") {
        RegionBuilder builder;
        builder.addChunk(0, 0, Compression::None, payloadOfLength(16, 1));
        // Claim 100 KiB inside a single 4 KiB sector.
        writeBigEndian32(builder.bytes(), 2U * kSectorBytes, 100U * 1024U);
        const RegionFile region = builder.build();
        CHECK_THROWS_AS(region.readChunk(0, 0), FormatError);
    }

    SECTION("a zero-length payload") {
        RegionBuilder builder;
        builder.addChunk(0, 0, Compression::None, payloadOfLength(16, 1));
        writeBigEndian32(builder.bytes(), 2U * kSectorBytes, 0U);
        const RegionFile region = builder.build();
        CHECK_THROWS_AS(region.readChunk(0, 0), FormatError);
    }

    SECTION("a corrupt deflate stream") {
        RegionBuilder builder;
        builder.addChunk(0, 0, Compression::Zlib, payloadOfLength(512, 5));
        builder.bytes()[(2U * kSectorBytes) + 9U] = std::byte{0xFF};
        builder.bytes()[(2U * kSectorBytes) + 10U] = std::byte{0xFF};
        const RegionFile region = builder.build();
        CHECK_THROWS_AS(region.readChunk(0, 0), FormatError);
    }
}

TEST_CASE("unsupported compression schemes are named, never skipped", "[region][malformed]") {
    // SPEC §8: silently returning nothing for a chunk we cannot decode would
    // let a conformance run report parity it never checked.
    const std::vector<std::byte> payload = payloadOfLength(64, 2);

    SECTION("LZ4") {
        RegionBuilder builder;
        builder.addFramed(0, 0, 4, payload);
        const RegionFile region = builder.build();
        REQUIRE_THROWS_WITH(region.readChunk(0, 0), Catch::Matchers::ContainsSubstring("LZ4"));
    }

    SECTION("a custom scheme") {
        RegionBuilder builder;
        builder.addFramed(0, 0, 127, payload);
        const RegionFile region = builder.build();
        REQUIRE_THROWS_WITH(region.readChunk(0, 0), Catch::Matchers::ContainsSubstring("custom"));
    }

    SECTION("an unknown scheme") {
        RegionBuilder builder;
        builder.addFramed(0, 0, 42, payload);
        const RegionFile region = builder.build();
        REQUIRE_THROWS_WITH(region.readChunk(0, 0), Catch::Matchers::ContainsSubstring("42"));
    }

    SECTION("an externally stored chunk") {
        RegionBuilder builder;
        builder.addFramed(0, 0, 0x80U | 2U, payload);
        const RegionFile region = builder.build();
        REQUIRE_THROWS_WITH(region.readChunk(0, 0), Catch::Matchers::ContainsSubstring(".mcc"));
    }
}
