// Stratum — every vanilla biome resolves to a Bedrock id.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/mapping-sync already refuses to generate a table missing a vanilla
// biome (it raises at generation time). This is the same check made again
// independently, against the SAME worldgen/biome fixtures but through the
// compiled loader rather than the generator's own Python — so a bug in how
// the C++ side reads the generated table, not just a bug in generating it,
// fails a test.
#include <stratum/mapping/biome.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using stratum::data::ResourceLocation;
using stratum::mapping::bedrockBiomeId;
using stratum::mapping::biomeTableSize;

TEST_CASE("every biome this build's pinned version has resolves to a Bedrock id", "[mapping]") {
    const std::filesystem::path biomeDir =
        std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11" / "worldgen" / "biome";
    if (!std::filesystem::is_directory(biomeDir)) {
        SKIP("no worldgen/biome fixtures under " << STRATUM_FIXTURES_DIR
                                                 << "; run tools/fetch-vanilla first");
    }

    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(biomeDir)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const ResourceLocation id("minecraft", entry.path().stem().string());
        INFO("biome " << id.toString());
        CHECK(bedrockBiomeId(id).has_value());
        ++checked;
    }
    // Equal, not just non-zero: a table with a stale extra row (a biome
    // removed from a later vanilla version but never pruned from the pinned
    // mapping commit) would still pass a "checked > 0" style assertion.
    CHECK(checked == biomeTableSize());
    CHECK(checked > 0);
}
