// Stratum — the aquifer's fluid-update flag, against the server's own lists.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Spec Q8.1: alongside the substance the aquifer decides whether a placed
// fluid is marked for post-processing — stored in the chunk's
// `PostProcessing` list, one section's packed `x | y << 4 | z << 8` per
// entry (measured: every entry of 66 156 lands on a fluid under that
// packing, against a third on air or stone under the y/z swap) — and that
// mark is what makes the fluid tick, and flow, once the chunk loads.
//
// A chunk keeps its list until it ticks. Every probe force-loads an 8x8
// window from chunk (0, 0), and the window plus the ring of chunks around it
// tick and empty their lists — asserted below, which is what makes the rest
// usable. Every other chunk that got as far as the noise stage keeps its
// list exactly as generation left it, and in a probe world no carver or
// feature runs, so that list is the aquifer's flag and nothing else: the
// server's own answer, position by position.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/probe_corpus.hpp"
#include "support/probe_region.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Position = stratum::test::LocalPosition;
using stratum::test::ticked;
using stratum::test::untouched;
namespace aquifer = stratum::aquifer;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Score {
    long long chunks = 0;
    long long tickedChunksChecked = 0;
    long long serverMarks = 0;
    long long agree = 0;
    long long extra = 0;   ///< ours, not the server's
    long long missing = 0; ///< the server's, not ours
};

/// Walks one probe region: asserts every ticked chunk's list is empty, and
/// scores @p oursFor(cx, cz) against every untouched chunk's own list.
template<typename OursFor>
void scoreRegion(const std::filesystem::path& region, std::int32_t minY, OursFor&& oursFor,
                 Score& score) {
    const auto file = stratum::region::RegionFile::open(region);
    for (std::int32_t cz = 0; cz < 32; ++cz) {
        for (std::int32_t cx = 0; cx < 32; ++cx) {
            if (!file.hasChunk(cx, cz)) {
                continue;
            }
            const auto doc = stratum::nbt::read(file.readChunk(cx, cz));
            const std::string status = doc.root.at("Status").asString();
            const std::set<Position> server = stratum::test::postProcessingMarks(doc.root, minY);
            if (ticked(cx, cz)) {
                if (status == "minecraft:full") {
                    INFO("chunk " << cx << ", " << cz);
                    CHECK(server.empty()); // the premise the rest rests on
                    ++score.tickedChunksChecked;
                }
                continue;
            }
            if (!untouched(cx, cz, status)) {
                continue;
            }
            ++score.chunks;
            const std::set<Position> ours = oursFor(cx, cz);
            score.serverMarks += static_cast<long long>(server.size());
            for (const Position& p : ours) {
                if (server.contains(p)) {
                    ++score.agree;
                } else {
                    ++score.extra;
                }
            }
            for (const Position& p : server) {
                score.missing += ours.contains(p) ? 0 : 1;
            }
        }
    }
}

} // namespace

TEST_CASE("the fluid-update flag is the server's own, on the comb worlds",
          "[conformance][aquifer]") {
    // The `jv` arm: every router entry constant but vanilla's `barrier`, so
    // the flag's whole case analysis runs on the full path and Q6.2's.
    const std::filesystem::path tree = fixtures() / "worldgen";
    if (!std::filesystem::is_directory(tree) ||
        !std::filesystem::is_regular_file(fixtures() / "probes" / "comb_42" / "jv" / "r.0.0.mca")) {
        SKIP("no comb probes under " << (fixtures() / "probes")
                                     << "; generate them with "
                                        "tools/analysis/aquifer-comb-probe.sh --accept-eula");
    }
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    const auto barrierNode = overworld.router.at(stratum::settings::RouterEntry::Barrier);
    const auto constant = [](double value) {
        return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
    };

    Score total;
    for (const std::int64_t seed : {42LL, 7LL, 12345LL, 999LL}) {
        INFO("seed " << seed);
        const std::filesystem::path region =
            fixtures() / "probes" / ("comb_" + std::to_string(seed)) / "jv" / "r.0.0.mca";
        REQUIRE(std::filesystem::is_regular_file(region));
        stratum::test::requireFrozen(region.parent_path().parent_path(),
                                     "tools/analysis/aquifer-comb-probe.sh");
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(loaded.graph, noises);
        stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
        const auto barrier = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return interpreter.evaluate(barrierNode,
                                        stratum::density::Point{.x = x, .y = y, .z = z}, cache);
        };
        const aquifer::CentreSource centres{seed};
        aquifer::StatusCache statuses;
        const auto oursFor = [&](std::int32_t cx, std::int32_t cz) {
            std::set<Position> ours;
            for (std::int32_t y = -64; y < 320; ++y) {
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        const aquifer::SubstanceAt at = aquifer::computeSubstance(
                            centres,
                            aquifer::AquiferQuery{.x = (cx * 16) + lx,
                                                  .y = y,
                                                  .z = (cz * 16) + lz,
                                                  .density = -1.0,
                                                  .seaLevel = 63},
                            statuses, barrier, constant(0.5), constant(0.0), constant(-1.0),
                            constant(96.0), aquifer::NoDeepDark{});
                        if (at.fluidUpdate) {
                            ours.emplace(lx, y, lz);
                        }
                    }
                }
            }
            return ours;
        };
        scoreRegion(region, -64, oursFor, total);
    }
    INFO("chunks " << total.chunks << ", server marks " << total.serverMarks << ", agree "
                   << total.agree << ", extra " << total.extra << ", missing " << total.missing);
    REQUIRE(total.chunks == 4 * 63);
    REQUIRE(total.tickedChunksChecked == 4 * 81);
    CHECK(total.serverMarks == 255457);
    CHECK(total.extra == 0);
    CHECK(total.missing == 0);
}

TEST_CASE("the fluid-update flag on water resting on the lava sea, where the spec is silent",
          "[conformance][aquifer]") {
    // The water/lava probes put water on row lambda, the one row Q6.3 owns:
    // real barrier, floodedness and spread noise, constant lava and psl.
    // Past Q6.2 that exit ALWAYS marks (substance.hpp, `fluidUpdateFlag`) —
    // which no reading of the clean-room spec gives, and which this case is
    // the evidence for. Scored over the rows the waterlava case reads,
    // lambda - 1 to lambda + 40, on the two seeds whose rows hold water.
    const std::filesystem::path tree = fixtures() / "worldgen";
    if (!std::filesystem::is_directory(tree) ||
        !std::filesystem::is_regular_file(fixtures() / "probes" / "waterlava_s42" / "spec.json")) {
        SKIP("no water/lava probes under " << (fixtures() / "probes")
                                           << "; generate them with "
                                              "tools/analysis/aquifer-waterlava-probe.sh");
    }
    const auto pack = stratum::data::Pack::open(tree);
    stratum::density::Graph::Builder builder(pack);
    const auto barrierNode = builder.add({{"type", "minecraft:noise"},
                                          {"noise", "minecraft:aquifer_barrier"},
                                          {"xz_scale", 1.0},
                                          {"y_scale", 0.5}});
    const auto floodNode = builder.add({{"type", "minecraft:noise"},
                                        {"noise", "minecraft:aquifer_fluid_level_floodedness"},
                                        {"xz_scale", 1.0},
                                        {"y_scale", 0.67}});
    const auto spreadNode = builder.add({{"type", "minecraft:noise"},
                                         {"noise", "minecraft:aquifer_fluid_level_spread"},
                                         {"xz_scale", 1.0},
                                         {"y_scale", 0.7142857142857143}});
    const stratum::density::Graph graph = builder.release();
    const std::vector<stratum::data::ResourceLocation> wanted{
        stratum::data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        stratum::data::ResourceLocation::parse("minecraft:aquifer_fluid_level_floodedness"),
        stratum::data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};

    Score total;
    long long waterOverLavaMarked = 0;
    for (const char* probeName : {"waterlava_s42", "waterlava_s8675309"}) {
        INFO("probe " << probeName);
        const std::filesystem::path probe = fixtures() / "probes" / probeName;
        stratum::test::requireFrozen(probe, "tools/analysis/aquifer-waterlava-probe.sh");
        std::ifstream manifestFile(probe / "manifest.json");
        const auto seed = nlohmann::json::parse(manifestFile).at("seed").get<std::int64_t>();
        std::ifstream specFile(probe / "spec.json");
        const nlohmann::json spec = nlohmann::json::parse(specFile);
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, wanted, seed, stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(graph, noises);
        stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
        const auto read = [&](stratum::density::NodeIndex node) {
            return [&, node](std::int32_t x, std::int32_t y, std::int32_t z) {
                return interpreter.evaluate(node, stratum::density::Point{.x = x, .y = y, .z = z},
                                            cache);
            };
        };
        const auto barrier = read(barrierNode);
        const auto flood = read(floodNode);
        const auto spread = read(spreadNode);
        const aquifer::CentreSource centres{seed};

        for (const auto& entry : spec) {
            const std::string name = entry.at("name").get<std::string>();
            INFO("dimension " << name);
            const double density = entry.at("raw_final_density").at("argument").get<double>();
            const auto seaLevel = entry.at("sea_level").get<std::int32_t>();
            const auto minY = entry.at("min_y").get<std::int32_t>();
            const double psl = entry.at("router").at("preliminary_surface_level").get<double>();
            const double lava = entry.at("router").at("lava").get<double>();
            const auto constant = [](double value) {
                return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
            };
            const std::int32_t lambda = aquifer::lambdaLevel(seaLevel);
            aquifer::StatusCache statuses;
            const auto oursFor = [&](std::int32_t cx, std::int32_t cz) {
                std::set<Position> ours;
                for (std::int32_t y = lambda - 1; y <= lambda + 40; ++y) {
                    for (int lz = 0; lz < 16; ++lz) {
                        for (int lx = 0; lx < 16; ++lx) {
                            const std::int32_t x = (cx * 16) + lx;
                            const std::int32_t z = (cz * 16) + lz;
                            const aquifer::SubstanceAt at = aquifer::computeSubstance(
                                centres,
                                aquifer::AquiferQuery{.x = x,
                                                      .y = y,
                                                      .z = z,
                                                      .density = density,
                                                      .seaLevel = seaLevel},
                                statuses, barrier, flood, spread, constant(lava), constant(psl),
                                aquifer::NoDeepDark{});
                            if (at.fluidUpdate) {
                                ours.emplace(lx, y, lz);
                                waterOverLavaMarked += y == lambda ? 1 : 0;
                            }
                        }
                    }
                }
                return ours;
            };
            // Only the scored rows of the server's list count.
            Score dimension;
            scoreRegion(
                probe / name / "r.0.0.mca", minY,
                [&](std::int32_t cx, std::int32_t cz) { return oursFor(cx, cz); }, dimension);
            const auto file = stratum::region::RegionFile::open(probe / name / "r.0.0.mca");
            long long serverOutsideRows = 0;
            for (std::int32_t cz = 0; cz < 32; ++cz) {
                for (std::int32_t cx = 0; cx < 32; ++cx) {
                    if (!file.hasChunk(cx, cz)) {
                        continue;
                    }
                    const auto doc = stratum::nbt::read(file.readChunk(cx, cz));
                    if (!untouched(cx, cz, doc.root.at("Status").asString())) {
                        continue;
                    }
                    for (const Position& p : stratum::test::postProcessingMarks(doc.root, minY)) {
                        const int y = std::get<1>(p);
                        serverOutsideRows += (y < lambda - 1 || y > lambda + 40) ? 1 : 0;
                    }
                }
            }
            total.chunks += dimension.chunks;
            total.agree += dimension.agree;
            total.extra += dimension.extra;
            total.missing += dimension.missing - serverOutsideRows;
            total.serverMarks += dimension.serverMarks - serverOutsideRows;
        }
    }
    INFO("chunks " << total.chunks << ", server marks in the rows " << total.serverMarks
                   << ", agree " << total.agree << ", extra " << total.extra << ", missing "
                   << total.missing << ", marked on row lambda " << waterOverLavaMarked);
    REQUIRE(total.chunks > 400);
    CHECK(total.extra == 0);
    CHECK(total.missing == 0);
    // The rule has to have been exercised: row lambda carries thousands.
    CHECK(waterOverLavaMarked > 1000);
}

TEST_CASE("the filler marks exactly the fluid updates the server does, on real overworld settings",
          "[conformance][aquifer][terrain]") {
    // End to end through ChunkFiller, with vanilla's own overworld router —
    // the aquifer-on probe (ore veins off, as there).
    const std::filesystem::path tree = fixtures() / "worldgen";
    const std::filesystem::path region =
        fixtures() / "probes" / "aquifer-on" / "seed--1" / "r.0.0.mca";
    if (!std::filesystem::is_directory(tree) || !std::filesystem::is_regular_file(region)) {
        SKIP("no aquifer-on probe at " << region
                                       << "; generate it with "
                                          "tools/analysis/aquifer-on-probe.sh --accept-eula");
    }
    stratum::test::requireFrozen(region.parent_path(), "tools/analysis/aquifer-on-probe.sh");
    const auto pack = stratum::data::Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    auto overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));
    overworld.oreVeinsEnabled = false;
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), -1, stratum::density::RandomSource::Xoroshiro);
    const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, overworld);
    const auto oursFor = [&](std::int32_t cx, std::int32_t cz) {
        stratum::terrain::ChunkBuffer buffer(overworld.geometry);
        filler.fill(cx, cz, buffer);
        std::set<Position> ours;
        for (const auto& update : buffer.fluidUpdates()) {
            const bool fresh = ours.emplace(update.localX, update.y, update.localZ).second;
            CHECK(fresh); // each position marked once
            // Marked only where the aquifer placed fluid (Q8.1).
            const std::string block =
                buffer.at(update.localX, update.y, update.localZ).name.toString();
            CHECK((block == "minecraft:water" || block == "minecraft:lava"));
        }
        return ours;
    };
    Score total;
    scoreRegion(region, overworld.geometry.minY, oursFor, total);
    INFO("chunks " << total.chunks << ", server marks " << total.serverMarks << ", extra "
                   << total.extra << ", missing " << total.missing);
    REQUIRE(total.chunks == 63);
    CHECK(total.serverMarks == 1964);
    CHECK(total.extra == 0);
    CHECK(total.missing == 0);
}
