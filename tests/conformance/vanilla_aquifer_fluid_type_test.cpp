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
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

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
#include <vector>

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
    long long flowing = 0;   ///< flowing blocks set aside, not attributed
    long long intruding = 0; ///< source blocks above their nearest source's own level
    long long minorityAtContact = 0;
    long long minorityElsewhere = 0;
    std::string mixedSamples;
    std::string missSamples;
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
                 << region << "; generate it with tools/analysis/aquifer-comb-probe.sh");
        }
        stratum::test::requireFrozen(region.parent_path().parent_path(),
                                     "tools/analysis/aquifer-comb-probe.sh");

        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), world.seed,
            stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(
            loaded.graph, noises,
            stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                           .height = overworld.geometry.cellHeight()});
        const stratum::aquifer::CentreSource centres{world.seed};
        const auto file = stratum::region::RegionFile::open(region);
        stratum::test::GoldenRegion golden(region);

        // Per source: the y of every source block it is nearest to, and
        // whether that block is lava.
        std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>,
                 std::vector<std::tuple<std::int32_t, std::int32_t, std::int32_t, bool>>>
            owned;
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                // A probe region holds every chunk of its window: a missing one is
                // a broken corpus, not a smaller sample.
                REQUIRE(file.hasChunk(cx, cz));
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
                            // SOURCE blocks only: a flowing one is fluid the
                            // server's ticks moved after generating, from
                            // whichever body it left — not this source's.
                            if (stratum::test::fluidLevel(block) != 0) {
                                ++total.flowing;
                                continue;
                            }
                            const CellIndex cell = stratum::aquifer::selectSources(
                                                       centres, (cx * 16) + lx, y, (cz * 16) + lz)
                                                       .nearest()
                                                       .cell;
                            owned[{cell.x, cell.y, cell.z}].emplace_back((cx * 16) + lx, y,
                                                                         (cz * 16) + lz, isLava);
                        }
                    }
                }
            }
        }

        for (const auto& [key, blocks] : owned) {
            const auto [ix, iy, iz] = key;
            const CellIndex centre = centres.centreOf(ix, iy, iz);
            if (centre.y < globalLava) {
                continue; // nothing observable below the lava sea
            }
            const auto spreadAt =
                stratum::aquifer::spreadSample(CellIndex{.x = ix, .y = iy, .z = iz}, centre);
            const double spread = interpreter.evaluate(
                spreadNode, Point{.x = spreadAt.x, .y = spreadAt.y, .z = spreadAt.z});
            const stratum::aquifer::CellLevel cellLevel = stratum::aquifer::cellLevel(
                stratum::aquifer::CellFluid{.centreY = centre.y,
                                            .surface = stratum::aquifer::constantSurface(kSurface),
                                            .seaLevel = kSeaLevel,
                                            .floodedness = kFloodedness,
                                            .spread = spread});
            const std::int32_t level = cellLevel.level;
            // Only the blocks this source itself holds — below its own level.
            // A source block at or above it is another body's fluid that
            // reached this territory, which no type rule can be scored on.
            std::pair<long, long> tally{0, 0};
            for (const auto& [bx, y, bz, isLava] : blocks) {
                if (y >= level) {
                    ++total.intruding;
                    continue;
                }
                ++(isLava ? tally.first : tally.second);
            }
            if (tally.first == 0 && tally.second == 0) {
                continue;
            }
            if (tally.first > 0 && tally.second > 0) {
                ++total.mixed;
                // The minority fluid's blocks: does each touch what water
                // meeting lava leaves behind?
                const bool minorityIsWater = tally.second < tally.first;
                for (const auto& [bx, y, bz, isLava] : blocks) {
                    if (y >= level || isLava == minorityIsWater) {
                        continue;
                    }
                    bool contact = false;
                    for (const auto& [dx, dy, dz] :
                         {std::tuple{1, 0, 0}, std::tuple{-1, 0, 0}, std::tuple{0, 1, 0},
                          std::tuple{0, -1, 0}, std::tuple{0, 0, 1}, std::tuple{0, 0, -1}}) {
                        contact = contact || stratum::test::fluidContactBlock(
                                                 golden.blockAt(bx + dx, y + dy, bz + dz));
                    }
                    ++(contact ? total.minorityAtContact : total.minorityElsewhere);
                }
                if (total.mixed <= 18) {
                    total.mixedSamples += " [" + std::to_string(ix) + "," + std::to_string(iy) +
                                          "," + std::to_string(iz) + " lava " +
                                          std::to_string(tally.first) + " water " +
                                          std::to_string(tally.second) + "]";
                }
                continue;
            }

            const auto lavaAt = stratum::aquifer::lavaSample(centre);
            const double lava =
                interpreter.evaluate(lavaNode, Point{.x = lavaAt.x, .y = lavaAt.y, .z = lavaAt.z});
            const bool observed = tally.first > 0;
            const bool predicted =
                stratum::aquifer::fluidTypeOf(stratum::aquifer::FluidTypeAt{
                    .centreY = centre.y,
                    .level = level,
                    .seaLevel = kSeaLevel,
                    .lava = lava,
                    .fromNearSurface =
                        cellLevel.origin == stratum::aquifer::LevelOrigin::NearSurfaceSea}) ==
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
            if (predicted != observed) {
                total.missSamples += " [cell " + std::to_string(ix) + "," + std::to_string(iy) +
                                     "," + std::to_string(iz) + " centre y " +
                                     std::to_string(centre.y) + " level " + std::to_string(level) +
                                     " lava " + std::to_string(lava) + " observed " +
                                     (observed ? "lava " : "water ") +
                                     std::to_string(observed ? tally.first : tally.second) + "]";
            }
            total.nullAllDefault += static_cast<int>(!observed);
            total.pitch16 += static_cast<int>(asSixteen == observed);
        }
    }

    REQUIRE(total.sources > 2500);
    INFO("sources " << total.sources << " (lava " << total.lava << "), agree " << total.agree
                    << ", null " << total.nullAllDefault << ", on the spread's pitch "
                    << total.pitch16 << ", mixed " << total.mixed << ", flowing set aside "
                    << total.flowing << ", intruding " << total.intruding
                    << ", minority at contact " << total.minorityAtContact << ", elsewhere "
                    << total.minorityElsewhere << ";" << total.mixedSamples << "; misses"
                    << total.missSamples);

    // The type IS a property of the source. The "4% of sources hold both
    // fluids" this case once carried was attribution, not the rule: count a
    // source's own SOURCE blocks below its own level, and set aside flowing
    // blocks and blocks above that level (another body's fluid in this
    // territory), and on the frozen corpora 17 of 3177 sources still mix —
    // every one a lava body holding a few water sources, 12 of those blocks
    // beside the obsidian or cobblestone water leaves on meeting lava. That
    // part is flow, and a run-dependent amount of it survives freezing
    // (support/probe_corpus.hpp), so it is bounded rather than pinned (it
    // read 15 sources and 14 blocks before the corpora were frozen): under
    // 2% of sources, with room for the run-to-run movement of that remnant.
    CHECK(total.mixed * 50 < total.sources);
    // The other 8 are named, not explained (SPEC §11): all in comb_999, each
    // a water source with two or more horizontal water-source neighbours and
    // flowing water beside it. That is the shape of water that spread in —
    // and equally of any water body's interior, so it is not credited. The
    // count is the same frozen and unfrozen, which is what pins it.
    CHECK(total.minorityElsewhere == 8);

    // And every pure source is typed as the rule says: 3160 of 3160 frozen
    // (3162 of 3162 before), where the old attribution read 0.99873 — its
    // four misses were two sources' worth of another body's water, which the
    // rule was never asked about.
    CHECK(total.agree == total.sources);
    CHECK(total.agree > total.nullAllDefault);

    // And the horizontal pitch is the finding. On the spread's lattice the
    // same rule scores 0.9424 — BELOW the baseline, so it is not a near miss.
    CHECK(total.pitch16 < total.nullAllDefault);
}
