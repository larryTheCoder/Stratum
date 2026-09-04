// Stratum — every fluid body in a column, tagged with its aquifer cell.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Every earlier aquifer instrument took ONE number per world — the top of the
// deepest water body, say — and so could only ever see whichever body happened
// to be topmost. That is why the fluid level looked like it saturated in the
// preliminary surface and moved non-monotonically in the floodedness: the
// readout was jumping between populations, not tracking one. A column holds
// three to eight separate fluid bodies, and each belongs to a different cell.
//
// Build it against a configured tree, for example:
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       tools/analysis/aquifer-cells.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aquifer-cells
//   build/aquifer-cells .fixtures/1.21.11/probes/<probe>/<dimension>
//
// It wants a world made by tools/analysis/density-probe.sh with aquifers on
// and `raw_final_density` at a negative constant, so that no terrain exists and
// every boundary it reads belongs to the aquifer.
#include <stratum/aquifer/lattice.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace stratum;
namespace {
constexpr int kMinY = -64, kMaxY = 319;
struct Body { int top, bottom; bool lava; };
}
int main(int argc, char** argv) {
    const std::string dir = argv[1];
    // cell y index -> level -> how many columns reported it
    std::map<int, std::map<int, long long>> byCellY;
    std::map<int, long long> bodiesPerColumn;
    long long columns = 0, bodies = 0;
    const auto file = region::RegionFile::open(dir + "/r.0.0.mca");
    for (std::int32_t cz = 0; cz < 8; ++cz) for (std::int32_t cx = 0; cx < 8; ++cx) {
        if (!file.hasChunk(cx, cz)) continue;
        const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
        for (int lz = 0; lz < 16; ++lz) for (int lx = 0; lx < 16; ++lx) {
            const int X = cx * 16 + lx, Z = cz * 16 + lz;
            std::vector<Body> found;
            int top = INT32_MIN;
            bool lava = false;
            for (int y = kMaxY; y >= kMinY - 1; --y) {
                const auto* b = y >= kMinY ? ch.blockAt(lx, y, lz) : nullptr;
                const bool fluid = b && (b->name == "minecraft:water" || b->name == "minecraft:lava");
                if (fluid && top == INT32_MIN) { top = y; lava = b->name == "minecraft:lava"; }
                else if (!fluid && top != INT32_MIN) { found.push_back({top, y + 1, lava});
                                                       top = INT32_MIN; }
            }
            ++columns;
            ++bodiesPerColumn[static_cast<int>(found.size())];
            for (const Body& body : found) {
                ++bodies;
                // fluid fills strictly below its level, as sea_level 63 topping
                // the water at 62 established
                const int level = body.top + 1;
                ++byCellY[aquifer::cellOf(X, body.top, Z).y][level];
            }
        }
    }
    std::printf("%s: %lld columns, %lld fluid bodies\n", dir.c_str(), columns, bodies);
    std::printf("bodies per column: ");
    for (const auto& [n, c] : bodiesPerColumn) std::printf("%d:%lld ", n, c);
    std::printf("\n\ncell y | span        | bodies | levels seen (level:share)\n");
    for (const auto& [cy, levels] : byCellY) {
        long long n = 0;
        for (const auto& [l, c] : levels) n += c;
        if (n < 40) continue;
        std::printf("%6d | [%4d,%4d] | %6lld | ", cy, cy * 12, cy * 12 + 11, n);
        for (const auto& [l, c] : levels)
            if (100.0 * double(c) / double(n) >= 1.0)
                std::printf("%d:%.0f%% ", l, 100.0 * double(c) / double(n));
        // level = base + 3k with k in [-4, 3], so a full ladder spans 21 and
        // the ends pin the base from either side
        int lo = 1 << 30, hi = -(1 << 30);
        for (const auto& [l, c] : levels)
            if (100.0 * double(c) / double(n) >= 1.0) { lo = std::min(lo, l); hi = std::max(hi, l); }
        std::printf("| span %d..%d, base from top %d, from bottom %d\n", lo, hi, hi - 9, lo + 12);
    }
}
