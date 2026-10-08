// Stratum — the overworld, end to end, against the blocks the vanilla server wrote.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHY THIS FILE EXISTS. Until it did, no conformance case ran the SHIPPED
// overworld — `world::CompiledDimension`, the path both native bindings
// generate through, with aquifers, ore veins, real biomes and the whole
// 287-rule surface tree — against the eight golden overworld regions. Every
// aquifer case used a probe world with surface rules OFF, and the one
// surface-on aquifer case (golden_fill_aquifer_test.cpp) read four chunks that
// hold no lava. So when the surface pass turned every lava block the aquifer
// placed into deepslate — 127531 of the goldens' 127700, through `categorize`
// calling lava Solid and an unconditioned deepslate gradient repainting it —
// and painted the bottom of open water in the same band (4215 blocks), the
// suite stayed green. Measured, then fixed (SPEC §11): the first pass had all
// of those blocks right.
//
// WHAT IT COMPARES, per golden seed, on a fixed 4x4 grid of chunks (every
// eighth chunk on each axis of r.0.0 — 16 chunks, 1572864 blocks a seed,
// sized for the Debug build `ctest --preset conformance` runs):
//
//   raw       ChunkFiller with no surface rules — the aquifer's own decision —
//             in four categories (air, water, lava, solid)
//   shipped   CompiledDimension::fillBlocks, block for block by name
//
// and two invariants that hold on every block, asserted per seed rather than
// as a rate: the surface pass never writes over a fluid the first pass
// placed, and every lava block the first pass got right survives it.
//
// The totals are pinned EXACTLY. They are not all 100%: the raw residual is
// the aquifer's own open items (fluid extent, and a deep-dark override the
// spec names — SPEC §11, PROGRESS.md's MA section) and the shipped residual
// is mostly surface MATERIAL near biome borders (M4). A pinned count is the
// point: any change to either, in either direction, fails here and has to be
// explained.
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Without
// them this skips; CI never generates regions, so it runs locally.
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {

using stratum::data::ResourceLocation;

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

constexpr std::array<std::int64_t, 8> kSeeds = {0,
                                                1,
                                                -1,
                                                42,
                                                2891948927356891LL,
                                                -4172144997902289642LL,
                                                9223372036854775807LL,
                                                -9223372036854775807LL - 1};

/// Every eighth chunk on each axis of r.0.0.
constexpr std::array<std::int32_t, 4> kChunkGrid = {0, 8, 16, 24};

enum class Category : std::uint8_t { Air, Water, Lava, Solid };

[[nodiscard]] Category categoryOf(const std::string& name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air") {
        return Category::Air;
    }
    if (name == "minecraft:water") {
        return Category::Water;
    }
    if (name == "minecraft:lava") {
        return Category::Lava;
    }
    return Category::Solid;
}

[[nodiscard]] bool isFluid(Category category) {
    return category == Category::Water || category == Category::Lava;
}

struct Score {
    std::size_t blocks = 0;
    /// The shipped pipeline's block NAME equals the golden's.
    std::size_t exact = 0;
    /// The raw first pass's category equals the golden's.
    std::size_t rawSameCategory = 0;
    std::size_t goldenLava = 0;
    /// Golden lava where the raw first pass also placed lava.
    std::size_t rawLavaRight = 0;
    /// ... and where the shipped pipeline still has it.
    std::size_t shippedLavaRight = 0;
    /// Positions where the first pass placed a fluid and the shipped output
    /// holds something else: the surface pass wrote over a fluid.
    std::size_t fluidOverwritten = 0;

    void absorb(const Score& other) {
        blocks += other.blocks;
        exact += other.exact;
        rawSameCategory += other.rawSameCategory;
        goldenLava += other.goldenLava;
        rawLavaRight += other.rawLavaRight;
        shippedLavaRight += other.shippedLavaRight;
        fluidOverwritten += other.fluidOverwritten;
    }
};

} // namespace

TEST_CASE("the shipped overworld against the golden regions: the aquifer survives the surface "
          "pass",
          "[conformance][aquifer][surface][world]") {
    const std::filesystem::path worldgen = versionDir() / "worldgen";
    const std::filesystem::path parameters = versionDir() / "biome_parameters";
    if (!std::filesystem::is_directory(worldgen / "noise_settings") ||
        !std::filesystem::is_regular_file(parameters / "minecraft" / "overworld.json")) {
        SKIP("no worldgen or biome_parameters fixtures under " << versionDir()
                                                               << "; run tools/fetch-vanilla");
    }
    const auto overworld = ResourceLocation::parse("minecraft:overworld");
    const stratum::data::Pack pack = stratum::data::Pack::open(worldgen);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& settings = loaded.settings.at(overworld);
    REQUIRE(settings.aquifersEnabled);
    REQUIRE(settings.oreVeinsEnabled);
    const auto blob = stratum::freeze::write(stratum::freeze::resolve(pack, parameters));

    Score total;
    std::size_t seedsScored = 0;
    for (const std::int64_t seed : kSeeds) {
        CAPTURE(seed);
        const std::filesystem::path region =
            versionDir() / "regions" / ("seed-" + std::to_string(seed)) / "overworld" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no golden overworld region at "
                 << region << "; generate it with tools/fetch-vanilla --generate-regions");
        }
        const auto shipped = stratum::world::CompiledDimension::compile(stratum::freeze::read(blob),
                                                                        overworld, overworld, seed);
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
        const auto raw = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings);
        const auto file = stratum::region::RegionFile::open(region);

        Score score;
        for (const std::int32_t cz : kChunkGrid) {
            for (const std::int32_t cx : kChunkGrid) {
                REQUIRE(file.hasChunk(cx, cz));
                const auto golden =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                stratum::terrain::ChunkBuffer first(settings.geometry);
                raw.fill(golden.x(), golden.z(), first);
                stratum::terrain::ChunkBuffer ours(shipped->geometry());
                shipped->fillBlocks(golden.x(), golden.z(), ours);

                for (std::int32_t y = first.minY(); y < first.minY() + first.height(); ++y) {
                    for (int z = 0; z < 16; ++z) {
                        for (int x = 0; x < 16; ++x) {
                            const stratum::chunk::BlockState* theirs = golden.blockAt(x, y, z);
                            REQUIRE(theirs != nullptr);
                            const std::string rawName = first.at(x, y, z).name.toString();
                            const std::string shippedName = ours.at(x, y, z).name.toString();
                            const Category goldenCategory = categoryOf(theirs->name);
                            const Category rawCategory = categoryOf(rawName);

                            ++score.blocks;
                            score.exact += static_cast<std::size_t>(shippedName == theirs->name);
                            score.rawSameCategory +=
                                static_cast<std::size_t>(rawCategory == goldenCategory);
                            if (goldenCategory == Category::Lava) {
                                ++score.goldenLava;
                                if (rawCategory == Category::Lava) {
                                    ++score.rawLavaRight;
                                    score.shippedLavaRight +=
                                        static_cast<std::size_t>(shippedName == "minecraft:lava");
                                }
                            }
                            if (isFluid(rawCategory) && shippedName != rawName) {
                                ++score.fluidOverwritten;
                            }
                        }
                    }
                }
            }
        }

        // THE TWO INVARIANTS, per seed: the surface pass writes only over the
        // default block.
        CHECK(score.fluidOverwritten == 0U);
        CHECK(score.shippedLavaRight == score.rawLavaRight);
        WARN("seed " << seed << ": shipped exact " << score.exact << " / " << score.blocks
                     << ", raw category " << score.rawSameCategory << ", golden lava "
                     << score.goldenLava << " (raw right " << score.rawLavaRight << ")");
        total.absorb(score);
        ++seedsScored;
    }

    REQUIRE(seedsScored == kSeeds.size());
    CHECK(total.blocks == 12582912U);
    // Every golden lava block on this grid is one the first pass got right,
    // and (the invariant above) every one of them survives the surface pass.
    // Before the fix the shipped count was 0: all of them were deepslate.
    CHECK(total.goldenLava == 1191U);
    CHECK(total.rawLavaRight == 1191U);
    // The aquifer's own residual on this grid: 23 of 12582912 categories,
    // the fluid-extent and deep-dark items (SPEC §11).
    CHECK(total.rawSameCategory == 12582889U);
    // The shipped residual, 540 blocks, is mostly surface MATERIAL near biome
    // borders (sand/dirt, sandstone/stone) — M4, not the aquifer.
    CHECK(total.exact == 12582372U);
}
