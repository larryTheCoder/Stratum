// Stratum — Anvil region (.mca) writer tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// writeRegion()'s own claim is round-trip fidelity against open()/readChunk()
// — independently tested against synthetic bytes (region_file_test.cpp) — so
// reading back what this writes is a genuine check, not a tautology.

#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::region::ChunkPayload;
using stratum::region::FormatError;
using stratum::region::RegionFile;
using stratum::region::writeRegion;

namespace {

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string& text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

[[nodiscard]] std::filesystem::path scratchPath(const char* name) {
    static int counter = 0;
    return std::filesystem::temp_directory_path() /
           ("stratum-region-writer-test-" + std::to_string(++counter) + "-" + name);
}

} // namespace

TEST_CASE("a region round-trips the chunks it was given, and nothing else", "[region][writer]") {
    const auto path = scratchPath("basic.mca");
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkPayload> chunks;
    chunks[{0, 0}] = ChunkPayload{.timestamp = 1000, .nbt = bytesOf("chunk at origin")};
    chunks[{5, 17}] = ChunkPayload{.timestamp = 2000, .nbt = bytesOf("a chunk somewhere else")};

    writeRegion(path, 0, 0, chunks);
    const auto region = RegionFile::open(path);

    CHECK(region.chunkCount() == 2U);
    REQUIRE(region.hasChunk(0, 0));
    CHECK(region.readChunk(0, 0) == bytesOf("chunk at origin"));
    CHECK(region.timestamp(0, 0) == 1000);
    REQUIRE(region.hasChunk(5, 17));
    CHECK(region.readChunk(5, 17) == bytesOf("a chunk somewhere else"));
    CHECK(region.timestamp(5, 17) == 2000);

    // Every other slot in the 32x32 region is absent, not merely unread.
    CHECK_FALSE(region.hasChunk(1, 0));
    CHECK(region.timestamp(1, 0) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("an empty region is a valid, openable file", "[region][writer]") {
    const auto path = scratchPath("empty.mca");
    writeRegion(path, 0, 0, {});
    const auto region = RegionFile::open(path);
    CHECK(region.chunkCount() == 0U);
    std::filesystem::remove(path);
}

TEST_CASE("a negative region writes chunks at their real coordinates", "[region][writer]") {
    const auto path = scratchPath("negative.mca");
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkPayload> chunks;
    chunks[{-32, -1}] = ChunkPayload{.timestamp = 1, .nbt = bytesOf("negative region chunk")};

    writeRegion(path, -1, -1, chunks);
    const auto region = RegionFile::open(path);
    REQUIRE(region.hasChunk(-32, -1));
    CHECK(region.readChunk(-32, -1) == bytesOf("negative region chunk"));

    std::filesystem::remove(path);
}

TEST_CASE("a chunk outside the declared region is refused, not silently misplaced",
          "[region][writer]") {
    const auto path = scratchPath("misplaced.mca");
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkPayload> chunks;
    chunks[{40, 0}] = ChunkPayload{.timestamp = 1, .nbt = bytesOf("wrong region")};

    CHECK_THROWS_AS(writeRegion(path, 0, 0, chunks), FormatError);
    CHECK_THROWS_WITH(writeRegion(path, 0, 0, chunks), ContainsSubstring("(40, 0)"));
}

TEST_CASE("padding sectors sit unowned between chunks and never change what reads back",
          "[region][writer]") {
    const auto path = scratchPath("padded.mca");
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkPayload> chunks;
    chunks[{0, 0}] = ChunkPayload{.timestamp = 111, .nbt = bytesOf("first")};
    chunks[{1, 0}] = ChunkPayload{.timestamp = 222, .nbt = bytesOf("second")};

    writeRegion(path, 0, 0, chunks, /*paddingSectors=*/3);
    const auto region = RegionFile::open(path);

    // Reading back is unaffected by padding: it is free space, not a
    // reinterpretation of anything the sector table points at.
    CHECK(region.readChunk(0, 0) == bytesOf("first"));
    CHECK(region.readChunk(1, 0) == bytesOf("second"));

    // Padding is exactly what makes the SECOND chunk's sector offset land
    // three sectors further out than it would with none — the whole point,
    // since that is the free space a server can grow the FIRST chunk into
    // without extending the file. sector 2 is the first past the header;
    // "first" is one sector (a few bytes rounds up to 4 KiB), so with
    // 3 padding sectors "second" starts at sector 2 + 1 + 3 = 6.
    const auto secondLocation = region.location(1, 0);
    CHECK(secondLocation.sectorOffset == 6U);

    std::filesystem::remove(path);
}
