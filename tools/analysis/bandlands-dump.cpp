// Stratum — dump a bandlands probe region as PNG slices and column strings.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Deliberately not a `stratum` subcommand: an instrument for one unsettled
// question (SPEC §11's `bandlands`), not a feature. tools/analysis/
// bandlands-probe.sh generates the region this reads; compile against the
// built library the same way tools/analysis/blended-probe.cpp does.
//
//   bandlands-dump <region.mca> <out-dir>
//
// Writes, under <out-dir>: a block-name histogram to stdout; one PNG per
// fixed z (a vertical x-vs-y slice) and one per sampled y (a horizontal
// x-vs-z slice); and columns.txt, the full top-to-bottom letter sequence at
// a handful of individual (x, z) columns.
#include <stratum/chunk/chunk.hpp>
#include <stratum/image/png.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int kMinY = -64;
constexpr int kHeight = 384;
constexpr int kSpan = 128; // 8 chunks * 16

// First-seen block name -> a stable letter and an RGB colour, both assigned
// on first encounter so the legend printed to stdout is the ground truth for
// every image written afterward.
struct Palette {
    std::vector<std::string> names;
    std::map<std::string, int> index;

    static constexpr std::array<std::array<std::uint8_t, 3>, 8> kColours{{
        {40, 40, 40},    // 0: whatever is first (expected: stone, unreplaced)
        {149, 87, 108},  // 1
        {161, 83, 37},   // 2
        {142, 60, 46},   // 3
        {208, 208, 208}, // 4
        {135, 107, 98},  // 5
        {80, 200, 120},  // 6 spare
        {230, 220, 60},  // 7 spare
    }};

    int of(const std::string& name) {
        auto it = index.find(name);
        if (it != index.end()) {
            return it->second;
        }
        const int i = static_cast<int>(names.size());
        names.push_back(name);
        index.emplace(name, i);
        return i;
    }

    [[nodiscard]] std::array<std::uint8_t, 3> colour(int i) const {
        return kColours[static_cast<std::size_t>(i) % kColours.size()];
    }

    char letter(int i) const { return static_cast<char>('A' + i); }
};

std::string blockAt(const stratum::chunk::Chunk& chunk, int localX, int y, int localZ) {
    const auto* block = chunk.blockAt(localX, y, localZ);
    return block != nullptr ? block->name : std::string("minecraft:air");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bandlands-dump <region.mca> <out-dir>\n");
        return 2;
    }
    const std::filesystem::path regionPath = argv[1];
    const std::filesystem::path outDir = argv[2];
    std::filesystem::create_directories(outDir);

    const auto region = stratum::region::RegionFile::open(regionPath);

    // Decode every chunk once into a flat grid of block names, kSpan x kSpan
    // x kHeight — about 6.3M short strings, which is a lot but not too much
    // for a one-off analysis run.
    std::vector<std::string> grid(static_cast<std::size_t>(kSpan) * kSpan * kHeight);
    auto at = [&](int x, int y, int z) -> std::string& {
        const auto ix = static_cast<std::size_t>(x);
        const auto iy = static_cast<std::size_t>(y - kMinY);
        const auto iz = static_cast<std::size_t>(z);
        return grid[((iy * kSpan) + iz) * kSpan + ix];
    };

    for (int chunkZ = 0; chunkZ < kSpan / 16; ++chunkZ) {
        for (int chunkX = 0; chunkX < kSpan / 16; ++chunkX) {
            if (!region.hasChunk(chunkX, chunkZ)) {
                std::fprintf(stderr, "missing chunk %d,%d\n", chunkX, chunkZ);
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(region.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const int x = (chunkX * 16) + localX;
                    const int z = (chunkZ * 16) + localZ;
                    for (int y = kMinY; y < kMinY + kHeight; ++y) {
                        at(x, y, z) = blockAt(chunk, localX, y, localZ);
                    }
                }
            }
        }
    }

    Palette palette;
    std::map<std::string, long long> histogram;
    for (const std::string& name : grid) {
        palette.of(name);
        ++histogram[name];
    }

    std::fprintf(stderr, "== histogram (%zu block names) ==\n", histogram.size());
    for (const auto& [name, count] : histogram) {
        std::fprintf(stderr, "  %c %-40s %lld\n", palette.letter(palette.of(name)), name.c_str(),
                     count);
    }

    // Vertical slices: x (0..127) across, y (min_y..max) down, one PNG per
    // sampled z.
    for (const int z : {0, 16, 32, 64, 96, 127}) {
        stratum::image::Image image(kSpan, kHeight);
        for (int y = kMinY; y < kMinY + kHeight; ++y) {
            for (int x = 0; x < kSpan; ++x) {
                const auto colour = palette.colour(palette.of(at(x, y, z)));
                // Row 0 is the TOP of the image; put the world's top at the
                // top of the picture, so y increases upward as it should.
                const auto row = static_cast<std::size_t>((kMinY + kHeight - 1) - y);
                image.setPixel(static_cast<std::size_t>(x), row, colour[0], colour[1], colour[2]);
            }
        }
        stratum::image::writePng(outDir / ("slice_z" + std::to_string(z) + ".png"), image);
    }

    // Horizontal slices: x across, z down, one PNG per sampled y.
    for (int y = kMinY; y < kMinY + kHeight; y += 16) {
        stratum::image::Image image(kSpan, kSpan);
        for (int z = 0; z < kSpan; ++z) {
            for (int x = 0; x < kSpan; ++x) {
                const auto colour = palette.colour(palette.of(at(x, y, z)));
                image.setPixel(static_cast<std::size_t>(x), static_cast<std::size_t>(z), colour[0],
                               colour[1], colour[2]);
            }
        }
        stratum::image::writePng(outDir / ("slice_y" + std::to_string(y) + ".png"), image);
    }

    // A handful of individual columns, top to bottom, as a letter string —
    // for spotting periodicity or an exact repeat by eye or by diffing.
    std::ofstream columns(outDir / "columns.txt");
    for (const auto& [x, z] : std::vector<std::pair<int, int>>{
             {0, 0}, {1, 0}, {0, 1}, {10, 0}, {0, 10}, {64, 64}, {64, 65}, {65, 64}, {127, 127}}) {
        columns << "x=" << x << " z=" << z << ": ";
        for (int y = kMinY + kHeight - 1; y >= kMinY; --y) {
            columns << palette.letter(palette.of(at(x, y, z)));
        }
        columns << "\n";
    }

    // The period-192 offset field: x=0,z=0's column, read off as one cycle,
    // is the hypothesis's canonical table; every other column is tested for
    // being an EXACT cyclic shift of it, and by how much.
    constexpr int kPeriod = 192;
    auto floorMod = [](int a, int m) { return ((a % m) + m) % m; };
    std::array<int, kPeriod> canonical{};
    for (int k = 0; k < kPeriod; ++k) {
        canonical[static_cast<std::size_t>(k)] = palette.of(at(0, kMinY + k, 0));
    }

    std::ofstream offsets(outDir / "offsets.csv");
    offsets << "x,z,shift\n";
    stratum::image::Image offsetImage(kSpan, kSpan);
    long long noMatch = 0;
    int minShift = kPeriod, maxShift = -1;
    for (int z = 0; z < kSpan; ++z) {
        for (int x = 0; x < kSpan; ++x) {
            int found = -1;
            for (int s = 0; s < kPeriod && found < 0; ++s) {
                bool ok = true;
                for (int y = kMinY; ok && y < kMinY + kPeriod; ++y) {
                    const int want = canonical[static_cast<std::size_t>(floorMod(y + s, kPeriod))];
                    if (palette.of(at(x, y, z)) != want) {
                        ok = false;
                    }
                }
                if (ok) {
                    found = s;
                }
            }
            offsets << x << "," << z << "," << found << "\n";
            if (found < 0) {
                ++noMatch;
                offsetImage.setPixel(static_cast<std::size_t>(x), static_cast<std::size_t>(z), 255, 0,
                                     0);
            } else {
                minShift = std::min(minShift, found);
                maxShift = std::max(maxShift, found);
                const auto v = static_cast<std::uint8_t>((found * 255) / (kPeriod - 1));
                offsetImage.setPixel(static_cast<std::size_t>(x), static_cast<std::size_t>(z), v, v,
                                     v);
            }
        }
    }
    stratum::image::writePng(outDir / "offsets.png", offsetImage);
    std::fprintf(stderr,
                 "== offset field: %lld/%d columns had no exact cyclic-shift match; shift range "
                 "[%d, %d] ==\n",
                 noMatch, kSpan * kSpan, minShift, maxShift);

    std::fprintf(stderr, "== wrote slices, columns.txt and offsets.{csv,png} under %s ==\n",
                 outDir.c_str());
    return 0;
}
