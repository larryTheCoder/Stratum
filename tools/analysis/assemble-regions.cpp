// Stratum — assembles region.mca files from generate-world.cpp's raw
// per-chunk output.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Deliberately not a `stratum` subcommand — see generate-world.cpp's own
// note. Splitting this out of that tool is what makes it safe to run many
// instances of it at once: each writes uniquely-named per-chunk files, never
// a region file, so any number of them can share one output directory with
// disjoint chunk ranges. This does the one thing that cannot be split —
// writeRegion() replaces a whole file per call — in a single pass, but that
// pass does no terrain computation, only I/O and zlib compression, so it
// costs a small fraction of what generating the chunks did.
//
//   assemble-regions <chunk-dir> <region-dir>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace stratum;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: assemble-regions <chunk-dir> <region-dir>\n");
        return 2;
    }
    const std::filesystem::path chunkDir = argv[1];
    const std::filesystem::path regionDir = argv[2];
    std::filesystem::create_directories(regionDir);

    const auto timestamp = static_cast<std::int32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    std::map<std::pair<std::int32_t, std::int32_t>,
             std::map<std::pair<std::int32_t, std::int32_t>, region::ChunkPayload>>
        byRegion;

    std::size_t read = 0;
    for (const auto& entry : std::filesystem::directory_iterator(chunkDir)) {
        if (entry.path().extension() != ".nbt") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        const std::size_t split = stem.find('_', 1); // 1: skip a leading '-' on cx
        if (split == std::string::npos) {
            std::fprintf(stderr, "skipping unrecognised file name: %s\n",
                        entry.path().filename().c_str());
            continue;
        }
        const std::int32_t cx = std::stoi(stem.substr(0, split));
        const std::int32_t cz = std::stoi(stem.substr(split + 1));

        std::ifstream in(entry.path(), std::ios::binary | std::ios::ate);
        const std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        // Every file here is nbt::write()'s own output (generate-world.cpp),
        // so this both validates it decodes cleanly and confirms the
        // eventual region entry is the same bytes a caller who read them
        // back would get — the same round-trip nbt_writer_test.cpp checks,
        // against real content instead of a hand-built one.
        (void)nbt::read(bytes);

        const std::int32_t regionX = javamath::floorDiv(cx, region::kChunksPerAxis);
        const std::int32_t regionZ = javamath::floorDiv(cz, region::kChunksPerAxis);
        byRegion[{regionX, regionZ}][{cx, cz}] =
            region::ChunkPayload{.timestamp = timestamp, .nbt = std::move(bytes)};
        ++read;
        if (read % 256 == 0) {
            std::fprintf(stderr, "read %zu chunk files\n", read);
        }
    }
    std::fprintf(stderr, "read %zu chunk files total\n", read);

    for (const auto& [region, chunks] : byRegion) {
        const auto& [regionX, regionZ] = region;
        const std::filesystem::path path =
            regionDir / ("r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca");
        // Every chunk here was written with no light data (chunk::encode's
        // own doc): a real server computes it the moment it first loads
        // one, which grows the chunk. Padding gives that growth somewhere
        // to land without extending the file itself — see writeRegion's own
        // doc for why that matters more than it sounds like it should.
        constexpr std::size_t kLightGrowthPaddingSectors = 4;
        region::writeRegion(path, regionX, regionZ, chunks, kLightGrowthPaddingSectors);
        std::fprintf(stderr, "wrote %s (%zu chunks)\n", path.c_str(), chunks.size());
    }

    return 0;
}
