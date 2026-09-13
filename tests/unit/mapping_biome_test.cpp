// Stratum — the generated Java -> Bedrock biome table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free on purpose: whether every vanilla biome at the pinned version
// resolves needs `tools/fetch-vanilla`'s own worldgen data and belongs in
// tests/conformance/ (vanilla_biome_mapping_test.cpp), which already has the
// skip-when-absent machinery this suite does not. What belongs here is
// pinning the loader's own behaviour against numbers read directly out of
// biomes.json (tools/mapping-sync's own header), and the shape of "not in
// the table" — a real path this library has to take without a fixture.
#include <stratum/mapping/biome.hpp>

#include <catch2/catch_test_macros.hpp>

using stratum::data::ResourceLocation;
using stratum::mapping::bedrockBiomeId;
using stratum::mapping::biomeTableSize;

TEST_CASE("a handful of real biomes resolve to the ids GeyserMC's table gives them", "[mapping]") {
    // Spot-checked against biomes.json directly, not recomputed here — a
    // generator bug that miscounted would not be caught by re-deriving the
    // same numbers from the same source.
    CHECK(bedrockBiomeId(ResourceLocation::parse("minecraft:plains")) == 1);
    CHECK(bedrockBiomeId(ResourceLocation::parse("minecraft:badlands")) == 37);
    CHECK(bedrockBiomeId(ResourceLocation::parse("minecraft:the_end")) == 9);
    CHECK(bedrockBiomeId(ResourceLocation::parse("minecraft:nether_wastes")) == 8);
    CHECK(bedrockBiomeId(ResourceLocation::parse("minecraft:deep_dark")) == 190);
}

TEST_CASE("a biome outside the table resolves to nothing, not a guess", "[mapping]") {
    // A custom/datapack biome the pinned GeyserMC commit cannot have heard
    // of. No fallback here yet (biome.hpp's own header says why); the
    // absence has to be visible to the caller, not silently defaulted.
    CHECK_FALSE(bedrockBiomeId(ResourceLocation::parse("examplemod:my_custom_biome")).has_value());
    CHECK_FALSE(bedrockBiomeId(ResourceLocation::parse("minecraft:not_a_real_biome")).has_value());
}

TEST_CASE("the generated table is exactly 65 rows at the pinned version", "[mapping]") {
    // Pinned as a number rather than left implicit, so a re-sync that
    // silently dropped or duplicated a row fails here even before the
    // conformance suite's own fixture-backed coverage check runs.
    CHECK(biomeTableSize() == 65);
}
