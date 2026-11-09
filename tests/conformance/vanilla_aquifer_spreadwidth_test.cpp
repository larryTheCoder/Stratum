// Stratum — the aquifer's spread quantization at 32-bit width, against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The golden confirmation (IMPLEMENTER-RULING-01 §2, aquifer P2) of Q5.7's
// width amendment in spec/aquifer-spec.md: the quantized term is `3 * F`, F the
// 32-bit floor of `(10.0 * s) / 3.0`, its product wrapping. The two readings
// this build has carried agree for every spread vanilla's routers make, so
// tools/analysis/aquifer-spreadwidth-probe.sh pushes a datapack's spread past
// the int range, where flooring `10 * s` first saturates and leaves the cells
// dry, and the amendment's form puts the ladder two blocks up (`sp`) or five
// down (`sn`). This replays each dimension through the shipped filler and
// requires the server's blocks, fluid where the amendment says.

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
#include <vector>

namespace {

constexpr const char* kScript = "tools/analysis/aquifer-spreadwidth-probe.sh";
constexpr const char* kCorpus = "aqspread_s42";
constexpr std::int64_t kSeed = 42;

/// Each arm and why it is there. Every arm holds fluid in the amendment's
/// form; the two scaled ones hold none if `10 * s` is floored first.
struct Arm {
    const char* name;
    const char* why;
};

constexpr std::array<Arm, 3> kArms{{
    {"sc", "spread 0.0: the ladder at 40b + 20 (control)"},
    {"sp", "spread 429496729.95: 3F wraps to 2, the ladder at 40b + 22"},
    {"sn", "spread -429496729.95: 3F wraps to -5, the ladder at 40b + 15"},
}};

[[nodiscard]] std::filesystem::path probeDir() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" / kCorpus;
}

class ScratchTree {
public:
    ScratchTree() : path_(stratum::test::tempPath("stratum-aquifer-spreadwidth")) {
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

TEST_CASE("the aquifer quantizes a spread past the int range as the server does",
          "[conformance][aquifer]") {
    if (!std::filesystem::is_regular_file(probeDir() / "spec.json")) {
        SKIP("no threshold probe at " << probeDir() << "; generate it with " << kScript
                                      << " --accept-eula " << kSeed
                                      << ", or tools/probe-worlds generate --only "
                                         "aqspread_s42 --accept-eula");
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

    std::vector<long long> fluidByArm;
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
        long long serverFluid = 0;
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
                            serverFluid +=
                                (served == "minecraft:water" || served == "minecraft:lava") ? 1 : 0;
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
                      << "; server fluid " << serverFluid);
        // Block for block, the server's.
        CHECK(wrong == 0);
        CHECK(exact + flow == blocks);
        // Flow is bounded, never pinned: the frozen worlds keep a
        // run-dependent remnant (SPEC §7), bounded as the threshold probe's.
        CHECK(flow * 100 <= blocks);
        // And the arm is not dry: what makes this corpus discriminate at all,
        // since flooring `10 * s` first leaves both scaled arms without fluid.
        CHECK(serverFluid > 0);
        fluidByArm.push_back(serverFluid);
    }
    // The scaled arms sit off the control's ladder (40b + 22 and 40b + 15
    // against 40b + 20), so each holds a different amount of fluid.
    REQUIRE(fluidByArm.size() == kArms.size());
    CHECK(fluidByArm[1] != fluidByArm[0]);
    CHECK(fluidByArm[2] != fluidByArm[0]);
}
