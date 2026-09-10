// Stratum — bandlands, end to end: this build's own Executor against the
// real blocks a probe region holds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Deliberately not a `stratum` subcommand: an instrument for one closed
// question (SPEC §11's `bandlands`, spec/bandlands-spec.md), kept as the
// most direct re-verification available rather than left to bit-rot. It
// compiles a real `surface::Executor` from a world seed and calls
// `bandlandsAt(x, y, z)` DIRECTLY against the block a
// tools/analysis/bandlands-probe.sh region holds at that position — no
// manual 192-entry table extraction, no phase derivation in between, just
// the whole implementation compared to the whole server output. This is
// what confirmed the fix for the pass (a) loop-boundary bug across three
// world seeds that the table-only check did not exercise widely enough to
// catch, and later confirmed all eight probed seeds exactly (1419264
// blocks, zero mismatches).
//
//   bandlands-e2e-check <region.mca> <seed>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace stratum;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bandlands-e2e-check <region.mca> <seed>\n");
        return 2;
    }
    const std::filesystem::path regionPath = argv[1];
    const std::int64_t seed = std::atoll(argv[2]);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "bandlands-e2e-check-pack";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path / "noise");
    {
        std::ofstream out(path / "noise" / "clay_bands_offset.json");
        out << R"({"firstOctave": -8, "amplitudes": [1.0]})";
    }
    const auto pack = data::Pack::openWorldgenTree(path);
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:clay_bands_offset")};
    const auto noises =
        density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);

    const surface::RuleGraph graph =
        surface::RuleGraph::resolve(nlohmann::json{{"type", "minecraft:bandlands"}},
                                    data::ResourceLocation::parse("stratum:test"));
    // Matches bandlands-probe.sh's own dimension shape; sea_level is unused
    // by bandlands itself (spec/bandlands-spec.md Q6.3) but Executor::compile
    // still takes one.
    const settings::NoiseGeometry geometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
    const surface::Executor executor = surface::Executor::compile(graph, seed, geometry, &noises, 63);

    const auto region = region::RegionFile::open(regionPath);

    std::size_t total = 0;
    std::size_t mismatches = 0;
    for (std::int32_t chunkZ = 0; chunkZ < 8 && mismatches < 20; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 8 && mismatches < 20; ++chunkX) {
            if (!region.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = chunk::Chunk::decode(nbt::read(region.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16 && mismatches < 20; localZ += 3) {
                for (int localX = 0; localX < 16 && mismatches < 20; localX += 3) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    for (std::int32_t y = -64; y < 320 && mismatches < 20; y += 5) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr) {
                            continue;
                        }
                        ++total;
                        const auto& ours = executor.bandlandsAt(x, y, z);
                        if (ours.name.toString() != block->name) {
                            std::printf("MISMATCH at (%d,%d,%d): ours=%s theirs=%s\n", x, y, z,
                                       ours.name.toString().c_str(), block->name.c_str());
                            ++mismatches;
                        }
                    }
                }
            }
        }
    }
    std::printf("%zu/%zu mismatches\n", mismatches, total);
    return mismatches > 0 ? 1 : 0;
}
