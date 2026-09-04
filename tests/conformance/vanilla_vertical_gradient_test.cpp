// Stratum — vertical_gradient's random source, scored on the server's blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Nineteen candidate derivations were refuted before this one, so the bar for
// believing the twentieth is high. The probe makes it cheap to clear: a
// dimension whose entire surface rule is ONE vertical_gradient painting a
// marker block turns every block of a chunk into a labelled outcome, and the
// probability at each height is known exactly from the two anchors. Chance is
// a coin flip; a correct source is every block.
//
// The bracket sweep is the sharper half. Its 55 bands share one draw per
// position and differ only in their threshold, so a source that is right has
// to predict all 55 from a single value — a source that is merely close fails
// on the bands whose threshold sits near its error.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace {

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Score {
    long long right = 0;
    long long total = 0;
    long long fired = 0;
};

// Every block of one probe world, against one gradient.
[[nodiscard]] Score scoreWorld(const std::filesystem::path& region, std::int64_t seed,
                               const std::string& randomName, std::int32_t trueAtAndBelow,
                               std::int32_t falseAtAndAbove, int yMax) {
    const auto source = stratum::rng::positionalSourceFor(seed, randomName);
    const auto file = stratum::region::RegionFile::open(region);
    Score score;
    for (std::int32_t cz = 0; cz < 8; ++cz) {
        for (std::int32_t cx = 0; cx < 8; ++cx) {
            if (!file.hasChunk(cx, cz))
                continue;
            const auto chunk =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    for (int y = 0; y <= yMax; ++y) {
                        const auto* block = chunk.blockAt(lx, y, lz);
                        if (block == nullptr)
                            continue;
                        const bool actual = block->name == "minecraft:diamond_block";
                        const bool predicted = stratum::surface::verticalGradientFires(
                            source, (cx * 16) + lx, y, (cz * 16) + lz, trueAtAndBelow,
                            falseAtAndAbove);
                        ++score.total;
                        score.right += static_cast<long long>(predicted == actual);
                        score.fired += static_cast<long long>(actual);
                    }
                }
            }
        }
    }
    return score;
}

} // namespace

TEST_CASE("vertical_gradient fires where the server fired it", "[conformance][surface]") {
    const std::filesystem::path single = fixtures() / "probes" / "cellsize" / "cell1" / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(single)) {
        SKIP("no vertical_gradient probe at "
             << single << "; generate it with tools/analysis/density-probe.sh");
    }

    const Score score = scoreWorld(single, 42, "minecraft:deepslate", -100, 124, 47);
    INFO("right " << score.right << " of " << score.total << ", fired " << score.fired);
    REQUIRE(score.total > 700000);
    CHECK(score.right == score.total);
    // The outcome is genuinely mixed, so agreement cannot be had by always
    // guessing one way.
    CHECK(score.fired > score.total / 4);
    CHECK(score.fired < (score.total * 3) / 4);
}

TEST_CASE("vertical_gradient holds across every band of the bracket sweep",
          "[conformance][surface]") {
    const std::filesystem::path root = fixtures() / "probes" / "tall";
    if (!std::filesystem::is_directory(root / "t00")) {
        SKIP("no bracket sweep at " << root
                                    << "; generate it with tools/analysis/density-probe.sh");
    }

    long long right = 0;
    long long total = 0;
    int bandsChecked = 0;
    for (int band = 0; band < 55; ++band) {
        char name[8];
        std::snprintf(name, sizeof name, "t%02d", band);
        const std::filesystem::path region = root / name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region))
            continue;
        // band k is true at and below k - 31 and false at and above k + 1
        const Score score = scoreWorld(region, 42, "minecraft:deepslate", band - 31, band + 1, 23);
        INFO("band " << name << ": " << score.right << " of " << score.total);
        CHECK(score.right == score.total);
        right += score.right;
        total += score.total;
        ++bandsChecked;
    }
    INFO("bands " << bandsChecked << ", right " << right << " of " << total);
    REQUIRE(bandsChecked >= 50);
    CHECK(right == total);
}
