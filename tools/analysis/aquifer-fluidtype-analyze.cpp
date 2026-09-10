// Stratum — reads back tools/analysis/aquifer-fluidtype-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// For each dimension the probe wrote, tags every fluid body in the probed
// chunks with its top y and whether it is lava, and reports the majority
// verdict — the probe holds `fluid_level_spread` at 0 and
// `fluid_level_floodedness` at 0.5, so every column in one dimension should
// agree once boundary noise (the barrier predicate's own per-cell draw)
// is averaged out.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       tools/analysis/aquifer-fluidtype-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aquifer-fluidtype-analyze
//   build/aquifer-fluidtype-analyze .fixtures/1.21.11/probes/fluidtype
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace stratum;

namespace {

constexpr int kMinY = -64;
constexpr int kMaxY = 319;

struct LevelCounts {
    long long water = 0;
    long long lava = 0;
};

struct Counts {
    long long water = 0;
    long long lava = 0;
    std::map<int, LevelCounts> byLevel; // top+1 -> water/lava split at that level
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: aquifer-fluidtype-analyze <probe-dir>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        const std::filesystem::path regionPath = root / name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(regionPath)) {
            continue;
        }
        const auto file = region::RegionFile::open(regionPath);
        Counts counts;
        for (std::int32_t cz = 0; cz < 8; ++cz) {
            for (std::int32_t cx = 0; cx < 8; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        int top = INT32_MIN;
                        bool lava = false;
                        for (int y = kMaxY; y >= kMinY - 1; --y) {
                            const auto* b = y >= kMinY ? ch.blockAt(lx, y, lz) : nullptr;
                            const bool fluid =
                                b && (b->name == "minecraft:water" || b->name == "minecraft:lava");
                            if (fluid && top == INT32_MIN) {
                                top = y;
                                lava = b->name == "minecraft:lava";
                            } else if (!fluid && top != INT32_MIN) {
                                const int level = top + 1;
                                LevelCounts& atLevel = counts.byLevel[level];
                                if (lava) {
                                    ++counts.lava;
                                    ++atLevel.lava;
                                } else {
                                    ++counts.water;
                                    ++atLevel.water;
                                }
                                top = INT32_MIN;
                            }
                        }
                    }
                }
            }
        }
        const long long total = counts.water + counts.lava;
        std::printf("%-12s total=%lld water=%lld lava=%lld", name.c_str(), total, counts.water,
                    counts.lava);
        if (total > 0) {
            std::printf(" (%.2f%% lava)", 100.0 * static_cast<double>(counts.lava) /
                                              static_cast<double>(total));
        }
        std::printf(" levels:");
        for (const auto& [level, at] : counts.byLevel) {
            const long long count = at.water + at.lava;
            if (count * 20 >= total) { // only print levels covering >=5% of bodies
                const char* verdict = at.lava == 0 ? "water" : (at.water == 0 ? "LAVA" : "MIXED");
                std::printf(" %d:%s(w%lld/l%lld)", level, verdict, at.water, at.lava);
            }
        }
        std::printf("\n");
    }
    return 0;
}
