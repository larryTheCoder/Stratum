// Stratum — the End, end to end, against the blocks the vanilla server wrote.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// THE END IS A LEGACY DIMENSION THAT NAMES NO NOISE. That is the whole reason
// this file can exist: `noise_settings/end.json` declares
// `legacy_random_source: true`, and walking its router and its surface rule
// finds zero `"noise": "<id>"` fields — pinned, per dimension and per router
// entry, by vanilla_legacy_named_noises_test.cpp. So the unsolved question
// next door (how a NAMED noise's identifier becomes a Java LCG seed, SPEC
// §11) is not on this dimension's critical path at all, and
// `NoiseRegistry::create` now refuses a legacy source only when it is
// actually asked for a name.
//
// WHAT THIS COMPARES. Every block of the window, at both granularities
// golden_fill_test.cpp uses. The End's geometry is min_y 0, height 128, so a
// chunk is 32768 blocks and the 8x8-chunk window is 2097152 of them.
//
// TWO CASES, because one fixture cannot reach the whole field.
//
// The first runs the eight golden `end/r.0.0` regions. Exact agreement there
// covers `BlendedNoise::legacyFromWorldSeed` reached through a real pipeline
// rather than a probe dimension (the End's `final_density` is
// `end/base_3d_noise`, an `old_blended_noise`, plus `end_islands`, and
// nothing else varies with the seed); `end_islands`' CENTRAL term, its `/8`
// grid and its `(h - 8) / 128` mapping; squeeze, blend_density, the
// interpolated cell lattice at 8x4; and the End's one-rule surface tree.
//
// It cannot cover the OUTER term or the simplex that gates it. r.0.0 is
// blocks 0..511 on both axes; the outer term is gated on
// `cellX^2 + cellZ^2 > 4096` with `cellX = blockX / 16`, and even the far
// corner of the +/-12 cell neighbourhood reaches only 43^2 * 2 = 3698. That
// is a geometric exclusion of the whole term, not a sampling gap. Nor can it
// tell floorDiv from truncation, being entirely non-negative. The second
// case runs the two probe regions that close both
// (tools/analysis/end-islands-probe.sh), and skips loudly when they are
// absent.
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Both are
// terrain-only worlds — carvers and features stripped by a generated
// datapack — which is what makes a block-by-block comparison against a
// density function meaningful at all.
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// Every seed tools/fetch-vanilla generates an End region for.
///
/// EIGHT REGIONS, SIX DISTINCT WORLDS — say the denominator, not the file
/// count. `java.util.Random` scrambles its seed as
/// `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, and that mask throws bit 63
/// away, so 0 and LONG_MIN collide and so do -1 and LONG_MAX. Everything in
/// the End goes through that LCG — the blended noise and the island
/// simplex alike — so those two pairs are the same world twice.
///
/// Not inferred from the arithmetic: the SERVER says so. The golden End
/// regions of 0 and LONG_MIN differ in 0 of 2097152 blocks, and so do
/// those of -1 and LONG_MAX. Which is itself worth having — it is an
/// independent confirmation that nothing in the End's terrain is seeded
/// from anything but the 48 bits the LCG keeps.
///
/// They are kept in the list anyway, because the collision is a claim about
/// this build as much as about vanilla: if the scramble were ever spelled
/// wrong here, the pairs would stop agreeing and these rows would catch it.
constexpr std::array<std::int64_t, 8> kSeeds = {0,
                                                1,
                                                -1,
                                                42,
                                                2891948927356891,
                                                -4172144997902289642,
                                                9223372036854775807,
                                                -9223372036854775807 - 1};

/// Chunks per axis of r.0.0 actually compared. The whole region is 32, and
/// the whole region is what the server generated; this is a cost dial, not a
/// coverage claim, and the denominator printed below always says which was
/// used.
constexpr std::int32_t kChunksPerAxis = 8;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

[[nodiscard]] std::string categoryOf(const std::string& name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air") {
        return "air";
    }
    if (name == "minecraft:water" || name == "minecraft:lava") {
        return "fluid";
    }
    return "solid";
}

struct Score {
    std::size_t blocks = 0;
    std::size_t exact = 0;
    std::size_t sameCategory = 0;
    std::size_t chunks = 0;
};

/// Scores a @p chunksPerAxis squared block of chunks, whose lowest chunk
/// coordinate is (@p fromChunkX, @p fromChunkZ), against @p filler. Chunk
/// coordinates rather than an offset into a region, because which corner of a
/// region carries the terrain differs: r.0.0's is its low corner and
/// r.-1.-1's is its HIGH one — the central island reaches only 100 blocks out,
/// so the first chunks of r.-1.-1 are 512 blocks away and hold nothing at all.
[[nodiscard]] Score scoreChunks(const std::filesystem::path& path,
                                const stratum::terrain::ChunkFiller& filler,
                                const stratum::settings::NoiseSettings& end,
                                std::int32_t fromChunkX, std::int32_t fromChunkZ,
                                std::int32_t chunksPerAxis) {
    const auto file = stratum::region::RegionFile::open(path);
    Score score;
    for (std::int32_t z = 0; z < chunksPerAxis; ++z) {
        for (std::int32_t x = 0; x < chunksPerAxis; ++x) {
            const std::int32_t chunkX = fromChunkX + x;
            const std::int32_t chunkZ = fromChunkZ + z;
            REQUIRE(file.hasChunk(chunkX, chunkZ));
            const auto golden = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);

            stratum::terrain::ChunkBuffer buffer(end.geometry);
            filler.fill(chunkX, chunkZ, buffer);
            ++score.chunks;

            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (std::int32_t y = end.geometry.minY;
                         y < end.geometry.minY + end.geometry.height; ++y) {
                        const std::string ours = buffer.at(localX, y, localZ).name.toString();
                        const auto* theirBlock = golden.blockAt(localX, y, localZ);
                        const std::string theirs =
                            theirBlock != nullptr ? theirBlock->name : std::string("minecraft:air");
                        ++score.blocks;
                        if (ours == theirs) {
                            ++score.exact;
                            ++score.sameCategory;
                        } else if (categoryOf(ours) == categoryOf(theirs)) {
                            ++score.sameCategory;
                        }
                    }
                }
            }
        }
    }
    return score;
}

/// Everything about the End that does not depend on the seed: the settings,
/// the rule tree, and the (empty) list of noises it needs.
struct EndUnderTest {
    stratum::data::Pack pack;
    stratum::settings::LoadedSettings loaded;
    stratum::settings::NoiseSettings settings;
    stratum::surface::RuleGraph rules;
    std::vector<stratum::data::ResourceLocation> wanted;
};

[[nodiscard]] EndUnderTest loadEnd() {
    auto pack = stratum::data::Pack::open(fixtures() / "worldgen");
    auto loaded = stratum::settings::loadAll(pack);
    const auto endId = stratum::data::ResourceLocation::parse("minecraft:end");
    auto settings = loaded.settings.at(endId);
    auto rules = stratum::surface::RuleGraph::resolve(settings.surfaceRule, endId);
    std::vector<stratum::data::ResourceLocation> wanted =
        loaded.graph.noisesReachableFrom(std::vector<stratum::density::NodeIndex>{
            settings.router.entries.begin(), settings.router.entries.end()});
    const auto surfaceNoises = stratum::surface::requiredNoises(rules);
    wanted.insert(wanted.end(), surfaceNoises.begin(), surfaceNoises.end());
    return EndUnderTest{std::move(pack), std::move(loaded), std::move(settings), std::move(rules),
                        std::move(wanted)};
}

} // namespace

TEST_CASE("the End generates the blocks the server wrote, on every golden seed",
          "[conformance][terrain][end][legacy]") {
    if (!std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no worldgen tree at " << (fixtures() / "worldgen") << "; run tools/fetch-vanilla");
    }
    const EndUnderTest end = loadEnd();

    // The premise, asserted rather than assumed: this is a legacy dimension,
    // and it is the two flags below that make the comparison a statement
    // about the density chain and the surface tree alone.
    REQUIRE(end.settings.legacyRandomSource);
    REQUIRE_FALSE(end.settings.aquifersEnabled);
    REQUIRE_FALSE(end.settings.oreVeinsEnabled);

    // The narrowing this file exists to exercise. If this is ever non-empty
    // the dimension is refused again, and it should be — with the names in
    // the message.
    CHECK(end.wanted.empty());

    const std::size_t expected = static_cast<std::size_t>(kChunksPerAxis) * kChunksPerAxis * 16U *
                                 16U * static_cast<std::size_t>(end.settings.geometry.height);
    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path region =
            fixtures() / "regions" / ("seed-" + std::to_string(seed)) / "end" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no golden End region at " << region << "; generate it with tools/fetch-vanilla");
        }

        const auto noises = stratum::density::NoiseRegistry::create(
            end.pack, end.wanted, seed, stratum::density::RandomSource::Legacy);
        const auto filler = stratum::terrain::ChunkFiller::compile(end.loaded.graph, noises,
                                                                   end.settings, &end.rules);
        // The End's tree is one unconditioned `block`, so nothing can block
        // it. Checked rather than assumed: a tree reported as blocked would
        // leave the default block everywhere and still score well here, since
        // the End's default block IS end_stone.
        CHECK(filler.runsSurfaceRules());
        CHECK(filler.surfaceRulesBlockedBy().empty());

        const Score score = scoreChunks(region, filler, end.settings, 0, 0, kChunksPerAxis);
        INFO("seed " << seed << ": " << score.exact << " of " << score.blocks << " blocks exact, "
                     << score.sameCategory << " same category");
        CHECK(score.chunks == static_cast<std::size_t>(kChunksPerAxis) * kChunksPerAxis);
        CHECK(score.blocks == expected);
        CHECK(score.sameCategory == expected);
        CHECK(score.exact == expected);
    }
}

TEST_CASE("the End's outer islands and its negative quadrant, where r.0.0 cannot reach",
          "[conformance][terrain][end][legacy][islands]") {
    // The two holes r.0.0 leaves in `end_islands`, and the only fixtures that
    // close them (see noise::EndIslands' header):
    //
    //   r.64.0   chunks 2048..2055, blocks 32768..32895 — far outside the
    //            `cellX^2 + cellZ^2 > 4096` gate, so this is the ONLY thing
    //            on disk that tests the outer-island term at all: its simplex
    //            field, its `< -0.9` gate, its steepness hash, and above all
    //            THE SEEDING of that simplex, which is otherwise a
    //            carried-over analogy rather than a measurement. It is
    //            visibly seed-dependent: 300318 end_stone blocks at seed 0
    //            against 670630 at seed 42, in the same 64 chunks.
    //   r.-1.-1  chunks -8..-1, blocks -128..-1, the HIGH corner of that
    //            region — where floorDiv/floorMod and Java's truncating `/`
    //            and `%` disagree, for the central island as much as for the
    //            outer one. Its low corner is 512 blocks out and holds
    //            nothing; see scoreChunks.
    //
    // Two seeds, not one: the outer islands are the seed-dependent half of
    // the field, and one seed agreeing with an RNG-driven reimplementation is
    // the coincidence this project has been caught by before.
    if (!std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no worldgen tree at " << (fixtures() / "worldgen") << "; run tools/fetch-vanilla");
    }
    const EndUnderTest end = loadEnd();

    struct Case {
        std::int64_t seed;
        std::int32_t regionX;
        std::int32_t regionZ;
        std::int32_t fromChunkX;
        std::int32_t fromChunkZ;
    };

    const std::array<Case, 4> cases = {
        Case{.seed = 0, .regionX = 64, .regionZ = 0, .fromChunkX = 2048, .fromChunkZ = 0},
        Case{.seed = 42, .regionX = 64, .regionZ = 0, .fromChunkX = 2048, .fromChunkZ = 0},
        Case{.seed = 0, .regionX = -1, .regionZ = -1, .fromChunkX = -8, .fromChunkZ = -8},
        Case{.seed = 42, .regionX = -1, .regionZ = -1, .fromChunkX = -8, .fromChunkZ = -8}};

    const std::size_t expected = static_cast<std::size_t>(kChunksPerAxis) * kChunksPerAxis * 16U *
                                 16U * static_cast<std::size_t>(end.settings.geometry.height);
    for (const Case& probe : cases) {
        const std::filesystem::path region =
            fixtures() / "probes" / "end-islands" / ("seed-" + std::to_string(probe.seed)) /
            ("r." + std::to_string(probe.regionX) + "." + std::to_string(probe.regionZ) + ".mca");
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no End-islands probe at " << region << "; generate it with "
                                            << "tools/analysis/end-islands-probe.sh --accept-eula");
        }

        const auto noises = stratum::density::NoiseRegistry::create(
            end.pack, end.wanted, probe.seed, stratum::density::RandomSource::Legacy);
        const auto filler = stratum::terrain::ChunkFiller::compile(end.loaded.graph, noises,
                                                                   end.settings, &end.rules);
        const Score score = scoreChunks(region, filler, end.settings, probe.fromChunkX,
                                        probe.fromChunkZ, kChunksPerAxis);
        INFO("seed " << probe.seed << " r." << probe.regionX << "." << probe.regionZ
                     << " from chunk (" << probe.fromChunkX << ", " << probe.fromChunkZ
                     << "): " << score.exact << " of " << score.blocks << " blocks exact");
        CHECK(score.blocks == expected);
        CHECK(score.exact == expected);
    }
}
