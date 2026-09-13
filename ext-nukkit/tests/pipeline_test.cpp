// Stratum — the Nukkit binding's own pipeline, without a JVM in the loop.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// jni_bridge.cpp itself is untested here — it needs a JVM to load and call
// into, which this suite deliberately does not require (this directory's
// own CMakeLists.txt splits the two for exactly this reason). What is
// tested is the boundary jni_bridge.cpp wraps: that compile() refuses a
// dimension this pipeline cannot build yet, and that fill() refuses rather
// than guesses at a block it has no Nukkit id for (SPEC §8) — the two
// pieces of ext-nukkit's own behaviour that do not need Java at all to
// verify.
#include <stratum_nukkit/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::nukkit::Pipeline;

namespace {
[[nodiscard]] std::filesystem::path packDir() {
    // The version root, matching Pipeline::compile's own documented shape
    // — NOT the worldgen/ tree directly.
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}
} // namespace

TEST_CASE("compile refuses a dimension this pipeline cannot build yet", "[nukkit]") {
    if (!std::filesystem::is_directory(packDir())) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    // Nether, End, caves and floating islands are all blocked on
    // legacy_random_source (M4) — this pipeline has never claimed to
    // support them, and compile() says so by name rather than trying and
    // producing a dimension whose density chain silently never ran.
    REQUIRE_THROWS_WITH(
        Pipeline::compile(packDir(), ResourceLocation::parse("minecraft:the_nether"), 1),
        ContainsSubstring("the_nether"));
}

TEST_CASE("compile succeeds for the overworld and reports its real geometry", "[nukkit]") {
    if (!std::filesystem::is_directory(packDir())) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const auto pipeline =
        Pipeline::compile(packDir(), ResourceLocation::parse("minecraft:overworld"), 1);
    // -64/384: vanilla's own overworld extent, and not incidentally the
    // same numbers CloudburstMC/Nukkit's own DimensionEnum.OVERWORLD
    // already uses (confirmed by reading Nukkit's source directly, not
    // assumed) — no height remapping is needed for this dimension.
    CHECK(pipeline->minY() == -64);
    CHECK(pipeline->height() == 384);
}

TEST_CASE("fill refuses the first block it cannot place, naming it, rather than guessing",
          "[nukkit]") {
    if (!std::filesystem::is_directory(packDir())) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const auto pipeline =
        Pipeline::compile(packDir(), ResourceLocation::parse("minecraft:overworld"), 1);
    std::vector<std::int32_t> fullBlockIds(static_cast<std::size_t>(pipeline->height()) * 16 * 16);
    std::vector<std::int32_t> biomeIds(static_cast<std::size_t>(pipeline->height() / 4) * 4 * 4);

    // No Java-state-to-Nukkit-legacy-id table has been sourced yet
    // (PROGRESS.md's M5 section), so every real column refuses today —
    // this pins that the refusal names the block rather than crashing or
    // silently writing zero into every entry.
    REQUIRE_THROWS_WITH(pipeline->fill(0, 0, fullBlockIds, biomeIds),
                        ContainsSubstring("no Nukkit block id mapping for"));
}

TEST_CASE("fill refuses a caller-sized output span that does not match this dimension's geometry",
          "[nukkit]") {
    if (!std::filesystem::is_directory(packDir())) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const auto pipeline =
        Pipeline::compile(packDir(), ResourceLocation::parse("minecraft:overworld"), 1);
    std::vector<std::int32_t> tooSmall(1);
    std::vector<std::int32_t> biomeIds(static_cast<std::size_t>(pipeline->height() / 4) * 4 * 4);

    REQUIRE_THROWS_WITH(pipeline->fill(0, 0, tooSmall, biomeIds),
                        ContainsSubstring("wrong size for this dimension"));
}
