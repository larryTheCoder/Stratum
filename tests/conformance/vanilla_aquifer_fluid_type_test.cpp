// Stratum — the aquifer's fluid type, scored on the server's own blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The `elava` probe arm is the only one this project ever built that gives
// the `lava` router entry vanilla's own noise instead of a constant. Every
// other probe pinned it at -1.0, which is why the fluid type went four
// campaigns without a single measurement: a corpus that holds constant the
// thing the answer depends on cannot answer it.
//
// THE READOUT IS PER SOURCE, not per block. Every fluid block is attributed
// to its rank-1 source, and a source is then one observation rather than its
// thousands of correlated blocks. That also makes the instrument falsifiable
// before any candidate is scored: if the type were not a property of the
// source, sources would come out holding both fluids.
//
// Two things are excluded, both for cause rather than convenience. Blocks
// below `min(-54, sea_level)` belong to the global lava sea, which overrides
// the lattice outright, so attributing them to a source manufactures a mixed
// reading out of every source that straddles the boundary. And sources whose
// own centre sits below that line cannot be observed at all.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>

namespace {

using stratum::aquifer::CellIndex;
using stratum::aquifer::FluidType;
using stratum::density::Point;

/// The `elava` arm: open void, aquifers on, `barrier`, `fluid_level_spread`
/// and `lava` all on vanilla's own noises, everything else constant.
constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kSurface = 96;
constexpr double kFloodedness = 0.5;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kMaxY = 200;

struct World {
    const char* dir;
    std::int64_t seed;
};

constexpr std::array<World, 4> kWorlds{
    {{"comb_42", 42}, {"comb_7", 7}, {"comb_12345", 12345}, {"comb_999", 999}}};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Score {
    long long sources = 0;
    long long mixed = 0;
    long long lava = 0;
    long long agree = 0;
    long long nullAllDefault = 0; ///< the majority baseline
    long long pitch16 = 0;        ///< the same rule on the spread's lattice
};

} // namespace

TEST_CASE("the aquifer's fluid is the type the server chose", "[conformance][aquifer]") {
    const std::filesystem::path tree = fixtures() / "worldgen";
    if (!std::filesystem::is_directory(tree)) {
        SKIP("no worldgen tree at " << tree << "; run tools/fetch-vanilla");
    }
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    // The probe's `lava` and `fluid_level_spread` entries are byte-identical
    // to vanilla's overworld, so the overworld's own router evaluates them.
    const auto lavaNode = overworld.router.at(stratum::settings::RouterEntry::Lava);
    const auto spreadNode = overworld.router.at(stratum::settings::RouterEntry::FluidLevelSpread);
    const std::int32_t globalLava = stratum::aquifer::lambdaLevel(kSeaLevel);

    Score total;
    for (const World& world : kWorlds) {
        const std::filesystem::path region =
            fixtures() / "probes" / world.dir / "elava" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no lava-noise aquifer probe at "
                 << region << "; generate it with tools/analysis/density-probe.sh");
        }

        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), world.seed,
            stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(
            loaded.graph, noises,
            stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                           .height = overworld.geometry.cellHeight()});
        const stratum::aquifer::CentreSource centres{world.seed};
        const auto file = stratum::region::RegionFile::open(region);

        std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::pair<long, long>> owned;
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        for (std::int32_t y = globalLava; y < kMaxY; ++y) {
                            const auto* block = chunk.blockAt(lx, y, lz);
                            if (block == nullptr) {
                                continue;
                            }
                            const bool isLava = block->name == "minecraft:lava";
                            if (!isLava && block->name != "minecraft:water") {
                                continue;
                            }
                            const CellIndex cell = stratum::aquifer::selectSources(
                                                       centres, (cx * 16) + lx, y, (cz * 16) + lz)
                                                       .nearest()
                                                       .cell;
                            auto& tally = owned[{cell.x, cell.y, cell.z}];
                            if (isLava) {
                                ++tally.first;
                            } else {
                                ++tally.second;
                            }
                        }
                    }
                }
            }
        }

        for (const auto& [key, tally] : owned) {
            const auto [ix, iy, iz] = key;
            if (tally.first > 0 && tally.second > 0) {
                ++total.mixed;
                continue;
            }
            const CellIndex centre = centres.centreOf(ix, iy, iz);
            if (centre.y < globalLava) {
                continue; // nothing observable below the lava sea
            }
            const auto spreadAt =
                stratum::aquifer::spreadSample(CellIndex{.x = ix, .y = iy, .z = iz}, centre);
            const double spread = interpreter.evaluate(
                spreadNode, Point{.x = spreadAt.x, .y = spreadAt.y, .z = spreadAt.z});
            const std::int32_t level = stratum::aquifer::cellFluidLevel(
                stratum::aquifer::CellFluid{.centreY = centre.y,
                                            .surface = stratum::aquifer::constantSurface(kSurface),
                                            .seaLevel = kSeaLevel,
                                            .floodedness = kFloodedness,
                                            .spread = spread});

            const auto lavaAt = stratum::aquifer::lavaSample(centre);
            const double lava =
                interpreter.evaluate(lavaNode, Point{.x = lavaAt.x, .y = lavaAt.y, .z = lavaAt.z});
            const bool observed = tally.first > 0;
            const bool predicted =
                stratum::aquifer::fluidTypeOf(stratum::aquifer::FluidTypeAt{
                    .centreY = centre.y, .level = level, .seaLevel = kSeaLevel, .lava = lava}) ==
                FluidType::Lava;

            // The null that matters, in the same loop: the identical rule
            // reading the SPREAD's lattice instead of lava's own.
            const double onSixteen = interpreter.evaluate(
                lavaNode, Point{.x = stratum::javamath::floorDiv(centre.x, 16),
                                .y = lavaAt.y,
                                .z = stratum::javamath::floorDiv(centre.z, 16)});
            const bool asSixteen =
                level <= stratum::aquifer::kLavaLevelCeiling &&
                (onSixteen < 0.0 ? -onSixteen : onSixteen) > stratum::aquifer::kLavaThreshold;

            ++total.sources;
            total.lava += static_cast<int>(observed);
            total.agree += static_cast<int>(predicted == observed);
            total.nullAllDefault += static_cast<int>(!observed);
            total.pitch16 += static_cast<int>(asSixteen == observed);
        }
    }

    REQUIRE(total.sources > 2500);
    INFO("sources " << total.sources << " (lava " << total.lava << "), agree " << total.agree
                    << ", null " << total.nullAllDefault << ", on the spread's pitch "
                    << total.pitch16 << ", mixed " << total.mixed);

    // The type IS a property of the source: sources holding both fluids are a
    // few per cent, not the norm. Those are unexplained and named as such in
    // SPEC §11 — the leading candidate is Q6.3's water-over-lava exception,
    // which no instrument in this project has yet touched.
    CHECK(total.mixed * 10 < total.sources);

    // 0.99873 when this was written, against a 0.94176 majority baseline.
    CHECK(total.agree * 1000 > total.sources * 997);
    CHECK(total.agree > total.nullAllDefault);

    // And the horizontal pitch is the finding. On the spread's lattice the
    // same rule scores 0.9424 — BELOW the baseline, so it is not a near miss.
    CHECK(total.pitch16 < total.nullAllDefault);
}
