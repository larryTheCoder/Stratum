// Stratum — the aquifer's thresholds in their last bits, against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The golden confirmation (IMPLEMENTER-RULING-01 §2) of two floating-point
// amendments to spec/aquifer-spec.md that no other corpus can see: Q5.6's
// level rule in the engine's form, whose lower threshold off the ramp is
// 0.40000000000000013 rather than 0.4, and Q5.9's deep-dark thresholds as
// single-precision literals widened to double. tools/analysis/
// aquifer-threshold-probe.sh writes seven constant dimensions, one per value
// that parts the readings (its header lists them); this replays each through
// the shipped filler, name for name, and requires each to land the way the
// engine's form says — dry, or flooded — where the decimal reading would put
// the other.

#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_settings.hpp"
#include "support/probe_spec.hpp"
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

constexpr const char* kScript = "tools/analysis/aquifer-threshold-probe.sh";
constexpr const char* kCorpus = "aqthreshold_s42";
constexpr std::int64_t kSeed = 42;

/// Each arm and what the engine's form makes of it: whether its cells hold
/// water (the ladder or the sea), or are dry.
struct Arm {
    const char* name;
    bool flooded;
    const char* why;
};

constexpr std::array<Arm, 7> kArms{{
    {"tc", true, "floodedness 0.5: the ladder (control)"},
    {"tb", false, "floodedness 0.4000000000000001: dry in the engine, the ladder at 0.4"},
    {"te", false, "floodedness 0.40000000000000013: the engine's threshold, strict"},
    {"ta", true, "floodedness 0.4000000000000002: past it, the ladder"},
    {"de", false, "erosion -0.225: deep dark in the engine, not at -0.225"},
    {"dd", false, "depth 0.9: deep dark in the engine, not at 0.9"},
    {"dc", true, "erosion -0.22499999403953552: the engine's threshold, strict: the sea"},
}};

[[nodiscard]] std::filesystem::path probeDir() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" / kCorpus;
}

class ScratchTree {
public:
    ScratchTree() : path_(stratum::test::tempPath("stratum-aquifer-threshold")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    ScratchTree(const ScratchTree&) = delete;
    ScratchTree& operator=(const ScratchTree&) = delete;
    ScratchTree(ScratchTree&&) = delete;
    ScratchTree& operator=(ScratchTree&&) = delete;

    ~ScratchTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("the aquifer's thresholds decide in the engine's last bits, as the server does",
          "[conformance][aquifer]") {
    if (!std::filesystem::is_regular_file(probeDir() / "spec.json")) {
        SKIP("no threshold probe at " << probeDir() << "; generate it with " << kScript
                                      << " --accept-eula " << kSeed
                                      << ", or tools/probe-worlds generate --only "
                                         "aqthreshold_s42 --accept-eula");
    }
    const nlohmann::json spec = stratum::test::readSpec(probeDir());
    stratum::test::requireFrozen(probeDir(), kScript);
    stratum::test::requireSeed(probeDir(), kSeed);

    std::ifstream manifestIn(probeDir() / "manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifestIn);
    const std::int32_t originX = manifest.at("origin_chunk").at(0).get<std::int32_t>();
    const std::int32_t originZ = manifest.at("origin_chunk").at(1).get<std::int32_t>();
    const auto regionName = manifest.at("region").get<std::string>();
    const std::int32_t regionX = originX >= 0 ? originX / 32 : -((-originX + 31) / 32);
    const std::int32_t regionZ = originZ >= 0 ? originZ / 32 : -((-originZ + 31) / 32);

    const ScratchTree scratch;
    for (const Arm& arm : kArms) {
        std::ofstream out(scratch.path() / "noise_settings" / (std::string(arm.name) + ".json"));
        out << stratum::test::probeNoiseSettings(
                   stratum::test::specEntry(spec, probeDir(), arm.name))
                   .dump();
    }
    const stratum::data::Pack pack = stratum::data::Pack::open(scratch.path());
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);

    for (const Arm& arm : kArms) {
        INFO("arm " << arm.name << " — " << arm.why);
        const stratum::data::ResourceLocation id{"minecraft", arm.name};
        const stratum::settings::NoiseSettings& dimension = loaded.settings.at(id);
        REQUIRE_FALSE(dimension.legacyRandomSource);
        const auto noises =
            stratum::density::NoiseRegistry::create(pack, loaded.graph.referencedNoises(), kSeed,
                                                    stratum::density::RandomSource::Xoroshiro);
        const stratum::surface::RuleGraph rules =
            stratum::surface::RuleGraph::resolve(dimension.surfaceRule, id);
        const stratum::terrain::ChunkFiller filler =
            stratum::terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);

        stratum::test::GoldenRegion region(probeDir() / arm.name / regionName, regionX, regionZ);
        stratum::terrain::ChunkBuffer buffer(dimension.geometry);
        long long blocks = 0;
        long long exact = 0;
        long long flow = 0;
        long long wrong = 0;
        long long serverWater = 0;
        // A 2x2 block of chunks inside the forceloaded 8x8 window.
        for (std::int32_t dz = 3; dz < 5; ++dz) {
            for (std::int32_t dx = 3; dx < 5; ++dx) {
                const std::int32_t chunkX = originX + dx;
                const std::int32_t chunkZ = originZ + dz;
                REQUIRE(region.hasChunk(chunkX, chunkZ));
                filler.fill(chunkX, chunkZ, buffer);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        for (std::int32_t y = dimension.geometry.minY;
                             y < dimension.geometry.maxY(); ++y) {
                            ++blocks;
                            const auto* theirs = region.blockAt(x, y, z);
                            REQUIRE(theirs != nullptr);
                            const std::string& served = theirs->name;
                            serverWater += served == "minecraft:water" ? 1 : 0;
                            const std::string ours = buffer.at(localX, y, localZ).name.toString();
                            if (served == ours) {
                                ++exact;
                                continue;
                            }
                            const auto g = stratum::test::categoryOf(served);
                            const auto r = stratum::test::categoryOf(ours);
                            if (g != r && stratum::test::explainedByFlow(region, x, y, z, g, r)) {
                                ++flow;
                                continue;
                            }
                            ++wrong;
                            if (wrong <= 8) {
                                UNSCOPED_INFO(arm.name << " (" << x << ", " << y << ", " << z
                                                       << "): server " << served << ", ours "
                                                       << ours);
                            }
                        }
                    }
                }
            }
        }
        INFO("exact " << exact << ", flow " << flow << ", wrong " << wrong << " of " << blocks
                      << "; server water " << serverWater);
        // Block for block, the server's.
        CHECK(wrong == 0);
        CHECK(exact + flow == blocks);
        // Flow is bounded, never pinned: the frozen worlds keep a
        // run-dependent remnant (SPEC §7). An arm flooded to the sea holds far
        // more open water than the far-cell probe's ladders, so its remnant
        // is larger (684 of 393 216 on the first run): bounded at 1 in 100.
        CHECK(flow * 100 <= blocks);
        // And the side of the threshold the engine's form puts the arm on:
        // what makes this corpus discriminate at all.
        if (arm.flooded) {
            CHECK(serverWater > 0);
        } else {
            CHECK(serverWater == 0);
        }
    }
}
