// Stratum — weird_scaled_sampler, measured from the vanilla server itself.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Vanilla's own changelog gives the formula:
//
//     abs(rarity * noise(x/rarity, y/rarity, z/rarity))
//
// but the numeric rarity ladders could not be taken from documentation at all.
// minecraft.wiki carries both of them and its two pages assign them to
// `type_1` and `type_2` in OPPOSITE order, so a build that trusted the wiki
// had a even chance of generating every cave in the world from the wrong
// ladder. This is the measurement that decided it, and it is the vanilla
// server that decided it (SPEC §7, §11).
//
// tools/analysis/density-probe.sh writes one dimension per input value, each
// with `K * flat_cache(weird_scaled_sampler{...}) + y_clamped_gradient` as its
// entire final_density and nothing else that can place a block, so each
// column's terrain height is a reading of the node. The spec probes every
// threshold at the value itself and at plus and minus 1e-7, which is what
// pins the comparison as a strict `<`.
//
// This does not FIT a rarity — it predicts one from rarityValueMapper() and
// checks the field vanilla wrote against it. A fit would only show that some
// rarity works; this shows that ours does.
//
// The fixture is Mojang-derived and never committed. Without it this skips.
#include <stratum/chunk/chunk.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path probeRoot() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11" / "probes" / "wss-edges";
}

} // namespace

TEST_CASE("the rarity ladders the server itself uses are the ones this build uses",
          "[conformance][rarity]") {
    const std::filesystem::path root = probeRoot();
    if (!std::filesystem::is_regular_file(root / "manifest.json")) {
        SKIP("no probe fixture at " << root
                                    << "; generate it with tools/analysis/density-probe.sh "
                                       "--accept-eula --spec <wss-edges.json>");
    }

    const auto manifest = nlohmann::json::parse(std::ifstream(root / "manifest.json"));
    const auto spec = nlohmann::json::parse(std::ifstream(root / "spec.json"));
    const double scale = manifest.at("k").get<double>();
    const double minY = manifest.at("min_y").get<double>();
    const double height = manifest.at("height").get<double>();
    const auto seed = manifest.at("seed").get<std::int64_t>();

    // The probe's noise, as the probe recorded it — not restated here, so the
    // two cannot drift apart.
    const auto& declared = manifest.at("probe_noise");
    const auto amplitudes = declared.at("amplitudes").get<std::vector<double>>();
    auto random = stratum::rng::XoroshiroPositionalFactory(seed).fromHashOf(
        declared.at("id").get<std::string>());
    const auto noise = stratum::noise::NormalNoise::create(
        random, declared.at("first_octave").get<int>(), amplitudes);

    // One block of terrain is worth this much of the function, and no reading
    // taken this way can beat half of it.
    const double quantum = 2.0 / height / scale;

    std::size_t probes = 0;
    std::size_t thresholdsCovered = 0;
    for (const auto& entry : spec) {
        const std::string name = entry.at("name").get<std::string>();
        if (!entry.contains("mapper")) {
            continue;
        }
        const auto mca = root / name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(mca)) {
            continue;
        }
        const std::string mapper = entry.at("mapper").get<std::string>();
        const double input = entry.at("input").get<double>();
        const double rarity = stratum::density::rarityValueMapper(mapper, input);
        if (entry.contains("delta") && entry.at("delta").get<double>() == 0.0) {
            ++thresholdsCovered;
        }

        const auto file = stratum::region::RegionFile::open(mca);
        std::size_t columns = 0;
        std::size_t agreeing = 0;
        double worst = 0.0;

        for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const stratum::chunk::Chunk chunk = stratum::chunk::Chunk::decode(
                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        // Cell corners only, inside the forceloaded area:
                        // final_density is interpolated across the cell
                        // lattice, and chunks past the forceload sit in the
                        // region only partly generated.
                        if ((x % 4) != 0 || (z % 4) != 0 || x > 127 || z > 127) {
                            continue;
                        }
                        int surface = static_cast<int>(minY) - 1;
                        for (int y = static_cast<int>(minY + height) - 1; y >= minY; --y) {
                            const stratum::chunk::BlockState* block =
                                chunk.blockAt(localX, y, localZ);
                            if (block != nullptr && block->name != "minecraft:air") {
                                surface = y;
                                break;
                            }
                        }
                        if (surface < minY) {
                            continue;
                        }
                        const double gradient = 1.0 - (2.0 * ((surface + 0.5) - minY) / height);
                        const double theirs = -gradient / scale;
                        const double ours =
                            std::abs(rarity * noise.sample(x / rarity, 0.0, z / rarity));
                        ++columns;
                        const double error = std::abs(ours - theirs);
                        worst = std::max(worst, error);
                        if (error <= 0.5 * quantum + 1e-12) {
                            ++agreeing;
                        }
                    }
                }
            }
        }

        CAPTURE(name, mapper, input, rarity);
        REQUIRE(columns == 1024U);
        // Every column, to within half a block — the floor of what the
        // measurement resolves. A wrong rarity is not a near miss here: the
        // ladders are 0.5 apart at the closest, and half a step changes both
        // the amplitude and the sampled position.
        CHECK(agreeing == columns);
        CHECK(worst <= 0.5 * quantum + 1e-12);
        ++probes;
    }

    // The whole spec, not whichever entries happened to be readable.
    CHECK(probes == 35U);
    // Seven thresholds — three for type_1, four for type_2 — each probed at
    // the value itself. If a future spec stopped covering one, the ladder
    // would be asserted with a hole in it.
    CHECK(thresholdsCovered == 7U);
}
