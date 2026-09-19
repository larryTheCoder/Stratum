// Stratum — ore veins, block for block against the vanilla server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The reference worlds come from `tools/analysis/ore-vein-probe.sh`: vanilla's
// own `vein_toggle`/`vein_ridged`/`vein_gap` router entries over a fully solid
// column (`raw_final_density` a positive constant), aquifers and ore veins
// both on, and a surface rule that never fires. Every block below the ceiling
// starts as stone, so "not stone" is exactly "the vein system placed
// something" — there is nothing else in the dimension that could have.
//
// WHAT THIS PINS. Not a rate and not a shape: the exact block, at the exact
// position, for every candidate the deterministic gate admits. That is the
// bar the derivation had to clear before `ore_veins_enabled` stopped being
// refused, and it is the one a wrong salt, a wrong threshold, a wrong draw
// order or a wrong block table all fail loudly.
//
// TWO SETS, and the split matters. `probes/orevein-multi` is the DISCOVERY
// set — the worlds the derivation was found against. `probes/orevein-heldout`
// is nine worlds generated afterwards and never used to fit anything. A
// derivation that scores 100% on the first and anything less on the second
// would be a fit, not a law; both read 100.000%.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::ore::Vein;
using stratum::ore::VeinBlock;
using stratum::ore::VeinInputs;
using stratum::ore::VeinSource;
using stratum::ore::VeinType;
using stratum::settings::RouterEntry;

/// The fixture tree's own version directory, found by locating the worldgen
/// pack inside it — the same way every other conformance case does, rather
/// than hard-coding a version this file would then have to be edited to bump.
[[nodiscard]] std::filesystem::path fixturesRoot() {
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path().parent_path();
        }
    }
    return {};
}

/// The block name this build says belongs at a placed vein position. Kept
/// here rather than reached for out of the filler because the filler's own
/// table is private to it; a divergence between the two would mean the
/// conformance number stopped describing what generation actually writes,
/// which `fills a probe chunk` below is what catches.
[[nodiscard]] std::string nameOf(const Vein& vein) {
    const bool copper = vein.type == VeinType::Copper;
    switch (vein.block) {
        case VeinBlock::Filler:
            return copper ? "minecraft:granite" : "minecraft:tuff";
        case VeinBlock::Ore:
            return copper ? "minecraft:copper_ore" : "minecraft:deepslate_iron_ore";
        case VeinBlock::RawBlock:
            return copper ? "minecraft:raw_copper_block" : "minecraft:raw_iron_block";
        case VeinBlock::None:
            break;
    }
    return "minecraft:stone";
}

/// Any block only the vein system places. The probe worlds are solid stone
/// otherwise, so one of these at a position the gate REJECTED would be a vein
/// this build cannot produce — the failure the scoring loop is blind to.
[[nodiscard]] bool isVeinBlock(const std::string& name) {
    return name == "minecraft:granite" || name == "minecraft:tuff" ||
           name == "minecraft:copper_ore" || name == "minecraft:deepslate_iron_ore" ||
           name == "minecraft:raw_copper_block" || name == "minecraft:raw_iron_block";
}

struct Score {
    long long rows = 0;
    long long agree = 0;
    long long placed = 0;
    /// Vein blocks the server wrote at a position the gate turned away. The
    /// scoring loop `continue`s past those without looking, so without this
    /// counter the case can detect a false POSITIVE and a wrong block type but
    /// never a false NEGATIVE — and the gate's NECESSITY would go unguarded.
    long long escaped = 0;
};

/// Whether the server wrote a vein block at this position — used only at
/// positions the gate turned away.
[[nodiscard]] long long serverVeinBlockAt(const stratum::chunk::Chunk& decoded, const int localX,
                                          const std::int32_t y, const int localZ) {
    const auto* actual = decoded.blockAt(localX, y, localZ);
    return (actual != nullptr && isVeinBlock(actual->name)) ? 1 : 0;
}

/// Walks one probe world, scoring every candidate position the deterministic
/// gate admits. `seed` is both the world seed and the directory name.
[[nodiscard]] Score scoreWorld(const std::filesystem::path& region, const std::int64_t seed,
                               const Pack& pack, const stratum::settings::LoadedSettings& loaded) {
    const auto& overworld = loaded.settings.at(ResourceLocation::parse("minecraft:overworld"));
    const auto toggleNode = overworld.router.at(RouterEntry::VeinToggle);
    const auto ridgedNode = overworld.router.at(RouterEntry::VeinRidged);
    const auto gapNode = overworld.router.at(RouterEntry::VeinGap);

    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
    stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                       .height = overworld.geometry.cellHeight()});
    stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
    const VeinSource veins(seed);

    Score score;
    const auto file = stratum::region::RegionFile::open(region);
    for (std::int32_t chunkZ = 0; chunkZ < 8; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 8; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto decoded = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    for (std::int32_t y = stratum::ore::kLowestVeinY;
                         y <= stratum::ore::kHighestVeinY; ++y) {
                        const stratum::density::Point point{.x = x, .y = y, .z = z};
                        const double toggle = interpreter.evaluate(toggleNode, point, cache);
                        if (!stratum::ore::clearsRichness(y, toggle)) {
                            score.escaped += serverVeinBlockAt(decoded, localX, y, localZ);
                            continue;
                        }
                        const double ridged = interpreter.evaluate(ridgedNode, point, cache);
                        if (ridged >= 0.0) {
                            score.escaped += serverVeinBlockAt(decoded, localX, y, localZ);
                            continue;
                        }
                        const double gap = interpreter.evaluate(gapNode, point, cache);
                        const Vein vein = veins.at(
                            x, y, z, VeinInputs{.toggle = toggle, .ridged = ridged, .gap = gap});
                        const auto* actual = decoded.blockAt(localX, y, localZ);
                        const std::string served =
                            actual == nullptr ? std::string("minecraft:air") : actual->name;
                        ++score.rows;
                        score.agree += (nameOf(vein) == served) ? 1 : 0;
                        score.placed += vein.placed() ? 1 : 0;
                    }
                }
            }
        }
    }
    return score;
}

/// Scores every seed of one probe set that is actually on disk.
[[nodiscard]] Score scoreSet(const std::string& set, const std::vector<std::int64_t>& seeds) {
    const auto worldgen = fixturesRoot() / "worldgen";
    const auto pack = Pack::open(worldgen);
    const auto loaded = stratum::settings::loadAll(pack);
    Score total;
    for (const std::int64_t seed : seeds) {
        const auto region =
            fixturesRoot() / "probes" / set / ("seed-" + std::to_string(seed)) / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const Score one = scoreWorld(region, seed, pack, loaded);
        // Per seed, not just in total: a seed contributing most of the rows
        // could otherwise carry a derivation that is wrong on the others.
        // Several probe seeds legitimately contribute zero candidate rows —
        // the gate is strict — and those are skipped rather than counted as
        // a pass.
        if (one.rows > 0) {
            INFO("set " << set << ", seed " << seed << ": " << one.agree << " of " << one.rows);
            CHECK(one.agree == one.rows);
        }
        // Checked on EVERY seed, including the ones contributing no candidate
        // row: a seed the gate turns away entirely is exactly where a vein the
        // server placed would hide.
        INFO("set " << set << ", seed " << seed << ": " << one.escaped
                    << " vein blocks at gate-rejected positions");
        CHECK(one.escaped == 0);
        total.rows += one.rows;
        total.agree += one.agree;
        total.placed += one.placed;
        total.escaped += one.escaped;
    }
    return total;
}

} // namespace

TEST_CASE("ore veins match the server on the discovery worlds", "[conformance][ore][vein]") {
    if (!std::filesystem::is_directory(fixturesRoot() / "probes" / "orevein-multi")) {
        SKIP("no ore-vein probe worlds; run tools/analysis/ore-vein-probe.sh --accept-eula");
    }
    const Score score = scoreSet("orevein-multi", {43, 100, 200, 300, 400});
    INFO("discovery: " << score.agree << " of " << score.rows);
    CHECK(score.rows > 30000);
    CHECK(score.agree == score.rows);
    // Guards the whole case against passing on a world where the gate admitted
    // rows but the vein system placed nothing, which would make agreement
    // trivially "all stone matches all stone".
    CHECK(score.placed > 20000);
    // The other direction, which the scoring loop alone cannot see: the gate is
    // NECESSARY as well as sufficient. Nothing the server placed sits outside
    // it.
    CHECK(score.escaped == 0);
}

TEST_CASE("ore veins match the server on worlds generated after the fact",
          "[conformance][ore][vein]") {
    if (!std::filesystem::is_directory(fixturesRoot() / "probes" / "orevein-heldout")) {
        SKIP("no held-out ore-vein probe worlds; run tools/analysis/ore-vein-probe.sh "
             "--accept-eula for seeds 1234 5150 13579 24680 31415 70707 86420 99991 777777");
    }
    const Score score =
        scoreSet("orevein-heldout", {1234, 5150, 13579, 24680, 31415, 70707, 86420, 99991, 777777});
    INFO("held out: " << score.agree << " of " << score.rows);
    CHECK(score.rows > 40000);
    CHECK(score.agree == score.rows);
    CHECK(score.placed > 25000);
    // The gate's necessity, on worlds that were never used to fit it.
    CHECK(score.escaped == 0);
}
