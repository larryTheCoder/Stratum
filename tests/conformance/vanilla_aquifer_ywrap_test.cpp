// Stratum — the aquifer's lattice points at the edges of the 12-bit y field,
// against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The golden confirmation (IMPLEMENTER-RULING-01 §2, aquifer P2) of Q3.3's
// width amendment in spec/aquifer-spec.md: a lattice point's y is retained
// as a signed 12-bit value, ((12j + v + 2048) mod 4096) - 2048, and the
// nearest-four distance and the status position read that retained value.
// Only a dimension reaching past y 2027 or below y -2029 sees it.
// tools/analysis/aquifer-ywrap-probe.sh builds the tallest one a dimension
// type allows, open and underground throughout, so each edge's blocks take
// the fluid of whichever points rank nearest; this replays it through the
// shipped filler on two seeds and requires the server's blocks in both edge
// bands. Keeping the true y instead ranks a wrapped point among the nearest
// four and puts the neighbouring ladder band's fluid there.
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

constexpr const char* kScript = "tools/analysis/aquifer-ywrap-probe.sh";
constexpr std::array<std::int64_t, 2> kSeeds{42, 31337};

/// The two edge bands, and how far each reaches: the blocks whose nearest
/// four can include a point of a cell that straddles the 12-bit range
/// (j = 170 at the top, j = -171 at the bottom), and a margin of cells
/// either side so a band also scores blocks the wrap cannot reach.
struct Band {
    const char* name;
    std::int32_t low;
    std::int32_t high; // exclusive
};

constexpr std::array<Band, 2> kBands{{
    {"top", 1990, 2032},
    {"bottom", -2032, -1990},
}};

[[nodiscard]] std::filesystem::path probeDir(std::int64_t seed) {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" /
           ("aqywrap_s" + std::to_string(seed));
}

class ScratchTree {
public:
    ScratchTree() : path_(stratum::test::tempPath("stratum-aquifer-ywrap")) {
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

TEST_CASE("the aquifer retains a lattice point's y in 12 bits as the server does",
          "[conformance][aquifer]") {
    if (!std::filesystem::is_regular_file(probeDir(kSeeds[0]) / "spec.json")) {
        SKIP("no y-wrap probe at " << probeDir(kSeeds[0]) << "; generate it with " << kScript
                                   << " --accept-eula <seed>, or tools/probe-worlds generate "
                                      "--only aqywrap_s42 --accept-eula");
    }
    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path dir = probeDir(seed);
        INFO("seed " << seed);
        REQUIRE(std::filesystem::is_regular_file(dir / "spec.json"));
        const nlohmann::json spec = stratum::test::readSpec(dir);
        stratum::test::requireFrozen(dir, kScript);
        stratum::test::requireSeed(dir, seed);

        std::ifstream manifestIn(dir / "manifest.json");
        const nlohmann::json manifest = nlohmann::json::parse(manifestIn);
        const std::int32_t originX = manifest.at("origin_chunk").at(0).get<std::int32_t>();
        const std::int32_t originZ = manifest.at("origin_chunk").at(1).get<std::int32_t>();
        const auto regionName = manifest.at("region").get<std::string>();
        const std::int32_t regionX = originX >= 0 ? originX / 32 : -((-originX + 31) / 32);
        const std::int32_t regionZ = originZ >= 0 ? originZ / 32 : -((-originZ + 31) / 32);

        const ScratchTree scratch;
        // The router names vanilla's own aquifer noises; their parameters
        // come from the fixtures, as the server reads them, never re-typed.
        const std::filesystem::path vanillaNoises =
            std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "worldgen" / "noise";
        std::filesystem::create_directories(scratch.path() / "noise");
        for (const char* noise : {"aquifer_barrier", "aquifer_fluid_level_spread"}) {
            const std::string file = std::string(noise) + ".json";
            REQUIRE(std::filesystem::is_regular_file(vanillaNoises / file));
            std::filesystem::copy_file(vanillaNoises / file, scratch.path() / "noise" / file);
        }
        {
            std::ofstream out(scratch.path() / "noise_settings" / "yw.json");
            out << stratum::test::probeNoiseSettings(stratum::test::specEntry(spec, dir, "yw"))
                       .dump();
        }
        const stratum::data::Pack pack = stratum::data::Pack::open(scratch.path());
        const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
        const stratum::data::ResourceLocation id{"minecraft", "yw"};
        const stratum::settings::NoiseSettings& dimension = loaded.settings.at(id);
        REQUIRE_FALSE(dimension.legacyRandomSource);
        REQUIRE(dimension.geometry.minY == -2032);
        REQUIRE(dimension.geometry.maxY() == 2032);
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
        const stratum::surface::RuleGraph rules =
            stratum::surface::RuleGraph::resolve(dimension.surfaceRule, id);
        const stratum::terrain::ChunkFiller filler =
            stratum::terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);

        stratum::test::GoldenRegion region(dir / "yw" / regionName, regionX, regionZ);
        stratum::terrain::ChunkBuffer buffer(dimension.geometry);

        struct Score {
            long long blocks = 0;
            long long exact = 0;
            long long flow = 0;
            long long wrong = 0;
            long long serverFluid = 0;
        };

        std::array<Score, kBands.size()> scores{};
        // The forceloaded 8x8 window's inner 6x6. Each chunk is filled once
        // and both bands are scored from that one fill.
        for (std::int32_t dz = 1; dz < 7; ++dz) {
            for (std::int32_t dx = 1; dx < 7; ++dx) {
                const std::int32_t chunkX = originX + dx;
                const std::int32_t chunkZ = originZ + dz;
                REQUIRE(region.hasChunk(chunkX, chunkZ));
                filler.fill(chunkX, chunkZ, buffer);
                for (std::size_t b = 0; b < kBands.size(); ++b) {
                    const Band& band = kBands[b];
                    Score& score = scores[b];
                    for (int localZ = 0; localZ < 16; ++localZ) {
                        for (int localX = 0; localX < 16; ++localX) {
                            const std::int32_t x = (chunkX * 16) + localX;
                            const std::int32_t z = (chunkZ * 16) + localZ;
                            for (std::int32_t y = band.low; y < band.high; ++y) {
                                ++score.blocks;
                                const auto* theirs = region.blockAt(x, y, z);
                                REQUIRE(theirs != nullptr);
                                const std::string& served = theirs->name;
                                score.serverFluid +=
                                    (served == "minecraft:water" || served == "minecraft:lava") ? 1
                                                                                                : 0;
                                const std::string ours =
                                    buffer.at(localX, y, localZ).name.toString();
                                if (served == ours) {
                                    ++score.exact;
                                    continue;
                                }
                                const auto g = stratum::test::categoryOf(served);
                                const auto r = stratum::test::categoryOf(ours);
                                if (g != r &&
                                    stratum::test::explainedByFlow(region, x, y, z, g, r)) {
                                    ++score.flow;
                                    continue;
                                }
                                ++score.wrong;
                                if (score.wrong <= 8) {
                                    UNSCOPED_INFO(band.name << " (" << x << ", " << y << ", " << z
                                                            << "): server " << served << ", ours "
                                                            << ours);
                                }
                            }
                        }
                    }
                }
            }
        }
        for (std::size_t b = 0; b < kBands.size(); ++b) {
            const Band& band = kBands[b];
            const Score& score = scores[b];
            INFO("band " << band.name << " [" << band.low << ", " << band.high << "): exact "
                         << score.exact << ", flow " << score.flow << ", wrong " << score.wrong
                         << " of " << score.blocks << "; server fluid " << score.serverFluid);
            CHECK(score.wrong == 0);
            CHECK(score.exact + score.flow == score.blocks);
            // Flow is bounded, never pinned: the frozen worlds keep a
            // run-dependent remnant (SPEC §7).
            CHECK(score.flow * 100 <= score.blocks);
            // Each band holds fluid at both ladder bands it spans.
            CHECK(score.serverFluid > 0);
        }
    }
}
