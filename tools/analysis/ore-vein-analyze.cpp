// Stratum — reads back tools/analysis/ore-vein-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// For every non-stone block, evaluates `vein_toggle`/`vein_ridged`/
// `vein_gap` at that position through this build's own density::Interpreter
// (the SAME real router entries the probe copied into the datapack) and
// checks each piece of the wiki's own documented algorithm against it:
// the y-range/sign/type gate, the vein_ridged membership gate, the
// vein_gap ore/filler gate and its |vein_toggle|-mapped probability, and
// the 2% raw-ore rate. Nothing here is assumed correct — every check
// reports its own pass/fail count.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/ore-vein-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/ore-vein-analyze
//   build/ore-vein-analyze .fixtures/1.21.11/probes/orevein-multi <seed1> <seed2> ...
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace stratum;

namespace {

constexpr int kMinY = -64;
constexpr int kMaxY = 319;

struct VeinBlock {
    std::int32_t x, y, z;
    std::string name;
};

struct Tally {
    long long total = 0;
    long long yInRange = 0;      // y in [-60, 51)
    long long typeSignOk = 0;    // copper: toggle>0 & y in [0,50]; iron: toggle<=0 & y in [-60,-8]
    long long ridgedNegative = 0; // every touched block should have ridged<0
    long long gapAboveNeg3 = 0;  // ore blocks specifically
    long long gapAtOrBelowNeg3 = 0; // filler blocks specifically
    long long rawCount = 0;
    long long oreCount = 0;
    long long richnessOk = 0; // |toggle| clears the documented 0.4-0.6 falloff gate
    std::map<std::string, long long> byName;

    // Among blocks with gap > -0.3 (i.e. eligible for ore at all), binned by
    // |vein_toggle| into tenths of [0.4, 0.6] — tests the documented mapped
    // probability (10%-30%) directly against the observed ore rate per bin.
    struct Bin {
        long long ore = 0;
        long long total = 0;
    };
    std::map<int, Bin> byToggleBin;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ore-vein-analyze <probe-root> <seed> [<seed> ...]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path fixturesWorldgen = root.parent_path().parent_path() / "worldgen";

    const auto pack = data::Pack::open(fixturesWorldgen);
    const auto loaded = settings::loadAll(pack);
    const auto& overworld = loaded.settings.at(data::ResourceLocation::parse("minecraft:overworld"));
    const auto toggleNodeIdx = overworld.router.at(settings::RouterEntry::VeinToggle);
    const auto ridgedNodeIdx = overworld.router.at(settings::RouterEntry::VeinRidged);
    const auto gapNodeIdx = overworld.router.at(settings::RouterEntry::VeinGap);

    Tally total;

    for (int argi = 2; argi < argc; ++argi) {
        const std::int64_t seed = std::atoll(argv[argi]);
        const std::filesystem::path region =
            root / ("seed-" + std::to_string(seed)) / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            std::fprintf(stderr, "skip: no region at %s\n", region.c_str());
            continue;
        }
        const auto noises = density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, density::RandomSource::Xoroshiro);
        density::Interpreter interp(
            loaded.graph, noises,
            density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                  .height = overworld.geometry.cellHeight()});
        density::Interpreter::CornerCache cache(interp.cacheSize());

        const auto file = region::RegionFile::open(region);
        for (std::int32_t cz = 0; cz < 8; ++cz) {
            for (std::int32_t cx = 0; cx < 8; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        for (int y = kMinY; y <= kMaxY; ++y) {
                            const auto* b = ch.blockAt(lx, y, lz);
                            if (b == nullptr || b->name == "minecraft:stone") {
                                continue;
                            }
                            const int x = cx * 16 + lx;
                            const int z = cz * 16 + lz;
                            ++total.total;
                            ++total.byName[b->name];

                            // Filler blocks (granite/tuff) carry no "copper"
                            // or "iron" substring at all — the vein's TYPE
                            // has to be read off the whole small vocabulary
                            // of blocks this system places, not guessed from
                            // one block's own name.
                            const bool isCopper = b->name.find("copper") != std::string::npos ||
                                                  b->name == "minecraft:granite";
                            const bool isIron = b->name.find("iron") != std::string::npos ||
                                                b->name == "minecraft:tuff";
                            const bool isRaw = b->name.find("raw_") != std::string::npos;
                            const bool isOre = b->name.find("_ore") != std::string::npos;
                            if (isRaw) {
                                ++total.rawCount;
                            }
                            if (isOre || isRaw) {
                                ++total.oreCount;
                            }

                            const bool yInRange = (y >= -60 && y < 51);
                            if (yInRange) {
                                ++total.yInRange;
                            }

                            const double toggle = interp.evaluate(
                                toggleNodeIdx, density::Point{.x = x, .y = y, .z = z}, cache);
                            const double ridged = interp.evaluate(
                                ridgedNodeIdx, density::Point{.x = x, .y = y, .z = z}, cache);
                            const double gap = interp.evaluate(
                                gapNodeIdx, density::Point{.x = x, .y = y, .z = z}, cache);

                            bool signOk = false;
                            if (isCopper && y >= 0 && y <= 50 && toggle > 0.0) {
                                signOk = true;
                            }
                            if (isIron && y >= -60 && y <= -8 && toggle <= 0.0) {
                                signOk = true;
                            }
                            if (signOk) {
                                ++total.typeSignOk;
                            }

                            if (ridged < 0.0) {
                                ++total.ridgedNegative;
                            }

                            if (isOre || isRaw) {
                                if (gap > -0.3) {
                                    ++total.gapAboveNeg3;
                                }
                            } else {
                                // filler (granite/tuff/diorite/etc — whatever
                                // is not stone and not ore/raw)
                                if (gap <= -0.3) {
                                    ++total.gapAtOrBelowNeg3;
                                }
                            }

                            // Richness: |toggle| must clear 0.6 at either
                            // y-range limit, falling linearly to 0.4 at 20
                            // blocks inside it (the documented reading).
                            if (signOk) {
                                const int lower = isCopper ? 0 : -60;
                                const int upper = isCopper ? 50 : -8;
                                const int distFromLimit =
                                    std::min(y - lower, upper - y);
                                const double required =
                                    0.6 - (0.2 * std::min(distFromLimit, 20) / 20.0);
                                if (std::abs(toggle) >= required) {
                                    ++total.richnessOk;
                                } else {
                                    static int richDumped = 0;
                                    if (richDumped < 20) {
                                        ++richDumped;
                                        std::fprintf(stderr,
                                                     "RICHNESS SHORTFALL seed=%lld (%d,%d,%d) %s "
                                                     "|toggle|=%.4f required=%.4f dist=%d\n",
                                                     static_cast<long long>(seed), x, y, z, b->name.c_str(),
                                                     std::abs(toggle), required, distFromLimit);
                                    }
                                }
                            }

                            // Mapped-probability check: among blocks eligible
                            // for ore (gap > -0.3), bin by |toggle| in
                            // [0.4, 0.6] (tenths) and compare the observed
                            // ore rate per bin against the documented
                            // [0.1, 0.3] linear map.
                            if (gap > -0.3) {
                                const double clamped =
                                    std::min(0.6, std::max(0.4, std::abs(toggle)));
                                const int bin = static_cast<int>((clamped - 0.4) * 100.0);
                                auto& b2 = total.byToggleBin[bin];
                                ++b2.total;
                                if (isOre || isRaw) {
                                    ++b2.ore;
                                }
                            }

                            if (!yInRange || !signOk || ridged >= 0.0) {
                                static int dumped = 0;
                                if (dumped < 20) {
                                    ++dumped;
                                    std::fprintf(stderr,
                                                 "ANOMALY seed=%lld (%d,%d,%d) %s toggle=%.4f "
                                                 "ridged=%.4f gap=%.4f yInRange=%d signOk=%d\n",
                                                 static_cast<long long>(seed), x, y, z, b->name.c_str(), toggle, ridged,
                                                 gap, yInRange, signOk);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::printf("total non-stone blocks: %lld\n", total.total);
    std::printf("  y in [-60,51): %lld (%.2f%%)\n", total.yInRange,
                100.0 * double(total.yInRange) / double(total.total ? total.total : 1));
    std::printf("  type/sign consistent: %lld (%.2f%%)\n", total.typeSignOk,
                100.0 * double(total.typeSignOk) / double(total.total ? total.total : 1));
    std::printf("  vein_ridged < 0: %lld (%.2f%%)\n", total.ridgedNegative,
                100.0 * double(total.ridgedNegative) / double(total.total ? total.total : 1));
    std::printf("  ore/raw blocks with gap>-0.3: %lld / %lld\n", total.gapAboveNeg3, total.oreCount);
    std::printf("  filler blocks with gap<=-0.3: %lld / %lld\n", total.gapAtOrBelowNeg3,
                total.total - total.oreCount);
    std::printf("  raw among ore+raw: %lld / %lld (%.3f%%)\n", total.rawCount, total.oreCount,
                100.0 * double(total.rawCount) / double(total.oreCount ? total.oreCount : 1));
    std::printf("  richness (|toggle| clears the 0.4-0.6 falloff): %lld / %lld\n",
                total.richnessOk, total.typeSignOk);
    std::printf("\nblock name breakdown:\n");
    for (const auto& [name, count] : total.byName) {
        std::printf("  %-40s %lld\n", name.c_str(), count);
    }
    std::printf("\nore rate by |vein_toggle| bin, among gap>-0.3 blocks "
                "(predicted: bin*1%% + 10%%):\n");
    for (const auto& [bin, b2] : total.byToggleBin) {
        const double rate = 100.0 * double(b2.ore) / double(b2.total ? b2.total : 1);
        std::printf("  toggle in [%.2f,%.2f): total=%-6lld ore=%-6lld observed=%.2f%% predicted=%d%%\n",
                    0.4 + bin / 100.0, 0.4 + (bin + 1) / 100.0, b2.total, b2.ore, rate, 10 + bin);
    }
    return 0;
}
