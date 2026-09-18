// Stratum — WHERE ore veins go, and what may paint over them.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `vanilla_ore_vein_test.cpp` pins the three random draws against a fully
// solid probe that runs no surface rule. That is what makes it a clean read
// on the RNG, and what makes it silent on both facts here. The reference is
// `tools/analysis/ore-vein-placement-probe.sh`, whose three dimensions share
// that probe's candidate set exactly — the vein router reads neither the
// density nor the surface rule — and differ only in what a candidate position
// held before the vein system looked at it, and in what ran afterwards.
//
//   * `lowcut`/`highcut` put the same candidates over solid ground AND over
//     air, by crossing `final_density` through zero inside a vein range.
//   * `repaint` runs an UNCONDITIONAL surface rule painting diamond_block, so
//     a candidate that still holds its vein block proves the surface system
//     did not touch it.
//
// The second is load-bearing rather than academic. The overworld's own
// `deepslate` rule is unconditionally true below y = -8 and iron's whole
// range is [-60, -8], so a filler that let surface rules win over vein blocks
// would erase every iron vein in the world — while every other golden test,
// all of which run with ore veins off, stayed green.
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
#include <functional>
#include <string>

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

[[nodiscard]] std::filesystem::path probeRoot() {
    return fixturesRoot() / "probes" / "oreveinplacement";
}

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

/// Walks one probe dimension, handing every candidate position to @p visit as
/// (what this build says belongs there, what the server actually wrote).
void forEachCandidate(const std::string& dimension, const std::int64_t seed,
                      const std::function<void(const Vein&, const std::string&)>& visit) {
    const auto pack = Pack::open(fixturesRoot() / "worldgen");
    const auto loaded = stratum::settings::loadAll(pack);
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

    const auto file = stratum::region::RegionFile::open(probeRoot() / dimension / "r.0.0.mca");
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
                            continue;
                        }
                        const double ridged = interpreter.evaluate(ridgedNode, point, cache);
                        if (ridged >= 0.0) {
                            continue;
                        }
                        const double gap = interpreter.evaluate(gapNode, point, cache);
                        const auto* actual = decoded.blockAt(localX, y, localZ);
                        visit(veins.at(x, y, z,
                                       VeinInputs{.toggle = toggle, .ridged = ridged, .gap = gap}),
                              actual == nullptr ? std::string("minecraft:air") : actual->name);
                    }
                }
            }
        }
    }
}

} // namespace

TEST_CASE("a vein replaces solid ground and never air", "[conformance][ore][vein]") {
    if (!std::filesystem::is_directory(probeRoot())) {
        SKIP("no ore-vein placement probe; run tools/analysis/ore-vein-placement-probe.sh "
             "--accept-eula 100");
    }
    long long air = 0;
    long long airTheChainWouldHavePlaced = 0;
    long long solid = 0;
    long long solidAgree = 0;
    // `lowcut` crosses zero inside iron's range and `highcut` inside copper's,
    // so between them both metals are seen over solid ground AND over air.
    for (const std::string& dimension : {std::string("lowcut"), std::string("highcut")}) {
        REQUIRE(std::filesystem::is_regular_file(probeRoot() / dimension / "r.0.0.mca"));
        forEachCandidate(dimension, 100, [&](const Vein& vein, const std::string& served) {
            if (served == "minecraft:air") {
                ++air;
                airTheChainWouldHavePlaced += vein.placed() ? 1 : 0;
            } else {
                ++solid;
                solidAgree += (nameOf(vein) == served) ? 1 : 0;
            }
        });
    }
    INFO("air " << air << " (the chain would have placed " << airTheChainWouldHavePlaced
                << "), solid " << solidAgree << " of " << solid);
    // The whole point: the chain WOULD have placed veins at a large number of
    // these, and the server placed none. A build that dropped the solid guard
    // still passes vanilla_ore_vein_test.cpp and fails here.
    CHECK(air > 20000);
    CHECK(airTheChainWouldHavePlaced > 10000);
    CHECK(solid > 10000);
    CHECK(solidAgree == solid);
}

TEST_CASE("a surface rule paints over stone but not over a vein block",
          "[conformance][ore][vein][surface]") {
    if (!std::filesystem::is_directory(probeRoot() / "repaint")) {
        SKIP("no ore-vein placement probe; run tools/analysis/ore-vein-placement-probe.sh "
             "--accept-eula 100");
    }
    long long veinKept = 0;
    long long veinPaintedOver = 0;
    long long stonePaintedOver = 0;
    long long stoneKept = 0;
    forEachCandidate("repaint", 100, [&](const Vein& vein, const std::string& served) {
        if (vein.placed()) {
            (served == nameOf(vein) ? veinKept : veinPaintedOver)++;
        } else {
            (served == "minecraft:diamond_block" ? stonePaintedOver : stoneKept)++;
        }
    });
    INFO("vein kept " << veinKept << ", painted over " << veinPaintedOver << "; stone painted over "
                      << stonePaintedOver << ", kept " << stoneKept);
    // Both halves matter. If the rule had not fired at all, `stonePaintedOver`
    // would be zero and "no vein was painted over" would prove nothing.
    CHECK(veinKept > 10000);
    CHECK(veinPaintedOver == 0);
    CHECK(stonePaintedOver > 1000);
    CHECK(stoneKept == 0);
}
