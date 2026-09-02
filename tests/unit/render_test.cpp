// Stratum — region rendering tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A rendered image is not evidence of parity, so these tests check the
// properties that make it useful anyway: that absent data is visibly absent
// rather than guessed at, and that the same input always paints the same
// pixels.

#include "support/region_builder.hpp"

#include <stratum/render/region_render.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using stratum::render::Mode;
using stratum::render::renderRegion;
using stratum::test::RegionBuilder;
using stratum::test::SectionSpec;
using stratum::test::uniformSection;

constexpr std::array<std::uint8_t, 3> kBackground{16U, 16U, 24U};

[[nodiscard]] SectionSpec terrainSection(int y, const std::string& block, int topHeight) {
    SectionSpec section;
    section.y = y;
    section.palette = {"minecraft:air", block};
    section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    for (int localY = 0; localY <= topHeight; ++localY) {
        for (std::size_t index = 0; index < 256U; ++index) {
            section.blocks[(static_cast<std::size_t>(localY) * 256U) + index] = 1;
        }
    }
    return section;
}

} // namespace

TEST_CASE("a region renders one pixel per column", "[render]") {
    RegionBuilder builder;
    builder.addChunk(0, 0, {terrainSection(0, "minecraft:stone", 7)});

    const stratum::image::Image image = renderRegion(builder.build(), Mode::Heightmap);
    CHECK(image.width() == 512U);
    CHECK(image.height() == 512U);

    // The one chunk that exists is drawn...
    CHECK(image.pixel(0, 0) != kBackground);
    CHECK(image.pixel(15, 15) != kBackground);
    // ...and everything else is left as "nothing was read here".
    CHECK(image.pixel(16, 0) == kBackground);
    CHECK(image.pixel(511, 511) == kBackground);
}

TEST_CASE("rendering is deterministic", "[render]") {
    RegionBuilder builder;
    builder.addChunk(2, 3, {terrainSection(0, "minecraft:stone", 4)});
    const stratum::region::RegionFile region = builder.build();

    // Colours come from hashing names, never from a random palette, so two
    // runs must agree byte for byte.
    for (const Mode mode : {Mode::Heightmap, Mode::Biome, Mode::Blocks}) {
        const stratum::image::Image first = renderRegion(region, mode);
        const stratum::image::Image second = renderRegion(region, mode);
        CHECK(first.pixels() == second.pixels());
    }
}

TEST_CASE("different modes paint different pictures", "[render]") {
    RegionBuilder builder;
    SectionSpec section = terrainSection(0, "minecraft:stone", 6);
    section.biomePalette = {"minecraft:plains"};
    builder.addChunk(0, 0, {section});
    const stratum::region::RegionFile region = builder.build();

    const stratum::image::Image heightmap = renderRegion(region, Mode::Heightmap);
    const stratum::image::Image blocks = renderRegion(region, Mode::Blocks);
    CHECK(heightmap.pixels() != blocks.pixels());
}

TEST_CASE("a chunk that cannot be decoded is left blank, not invented", "[render][malformed]") {
    RegionBuilder builder;
    builder.addChunk(0, 0, {terrainSection(0, "minecraft:stone", 3)});
    builder.addChunkPayload(1, 0, std::vector<std::byte>(64, std::byte{0x7F}));

    const stratum::image::Image image = renderRegion(builder.build(), Mode::Heightmap);
    CHECK(image.pixel(0, 0) != kBackground);
    CHECK(image.pixel(16, 0) == kBackground);
}

TEST_CASE("mode names round-trip", "[render]") {
    for (const char* name : {"heightmap", "biome", "blocks"}) {
        Mode mode = Mode::Heightmap;
        REQUIRE(stratum::render::parseMode(name, mode));
        CHECK(stratum::render::modeName(mode) == name);
    }
    Mode unused = Mode::Heightmap;
    CHECK_FALSE(stratum::render::parseMode("nonsense", unused));
    CHECK_FALSE(stratum::render::parseMode("", unused));
}
