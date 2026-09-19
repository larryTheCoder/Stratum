// Stratum — scores tools/analysis/end-islands-field-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Inverts every cell-corner column of each probed dimension into a reading of
// `minecraft:end_islands` at that column — see density-probe.sh's header for
// the trick — and prints the field beside what noise::EndIslands predicts.
//
// WHY A SEPARATE TOOL AND NOT A CONFORMANCE CASE. The central-island term is
// settled and has conformance cases (golden_end_test.cpp). The OUTER term is
// not, and what is wanted here is not a pass/fail but a LOOK: the field
// itself, at a window far enough out that the outer term is the whole of it,
// so that a wrong hypothesis can be told from a wrong seed. A conformance
// case cannot do that, and a scan that only prints an agreement percentage
// repeats the mistake the legacy-seed scan already made.
//
// WHAT IT PRINTS, per dimension:
//
//   * the inverted field over the probed window, on the 4-block corner
//     lattice, as a map;
//   * the same window under noise::EndIslands, for the same seed;
//   * agreement, at the resolution of the instrument — half a quantum, a
//     quantum being one block of terrain, 2 / height / (K * scale) — with
//     clipped columns excluded from the denominator rather than counted
//     against whichever candidate is wrong;
//   * and the LEGACY-versus-MODERN comparison, which is a question of its
//     own: if a `legacy_random_source` dimension and its flag-off control
//     read the same field, `end_islands` does not go through the dimension's
//     declared random source at all.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated -I build/dev/_deps/nlohmann_json-src/include tools/analysis/end-islands-analyze.cpp build/dev/lib/libstratum_core.a -lz -o build/end-islands-analyze
//   build/end-islands-analyze .fixtures/1.21.11/probes/endfield_s0_2048_0 0
//   build/end-islands-analyze .fixtures/1.21.11/probes/endfield_s0_2048_0 0 0 65536
#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/end_islands.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace stratum;

namespace {

// Must match tools/analysis/density-probe.sh.
constexpr double kK = 0.35;
constexpr double kMinY = -64.0;
constexpr double kHeight = 384.0;

struct Dimension {
    const char* name;
    bool legacy;
    double scale;
};

// The four dimensions end-islands-field-probe.sh writes, in its order.
constexpr std::array<Dimension, 4> kDimensions{{
    {"leg_fine", true, 3.0},
    {"leg_coarse", true, 1.0},
    {"mod_fine", false, 3.0},
    {"mod_coarse", false, 1.0},
}};

using Field = std::map<std::pair<std::int32_t, std::int32_t>, double>;

/// Every cell-corner column of a probe region, as a reading of the function.
/// `clipped` counts the columns the inversion cannot speak for — the ones
/// whose terrain reaches the floor or the ceiling of the probe world, where
/// the height no longer moves with the value.
[[nodiscard]] Field readColumns(const std::filesystem::path& region, double scale,
                                std::int32_t fromChunkX, std::int32_t fromChunkZ,
                                std::int32_t chunks, std::size_t& clipped) {
    Field out;
    const region::RegionFile file = region::RegionFile::open(region);
    for (std::int32_t cz = 0; cz < chunks; ++cz) {
        for (std::int32_t cx = 0; cx < chunks; ++cx) {
            const std::int32_t chunkX = fromChunkX + cx;
            const std::int32_t chunkZ = fromChunkZ + cz;
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const chunk::Chunk decoded =
                chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; localZ += 4) {
                for (int localX = 0; localX < 16; localX += 4) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    int surface = static_cast<int>(kMinY) - 1;
                    for (int y = static_cast<int>(kMinY + kHeight) - 1; y >= kMinY; --y) {
                        const chunk::BlockState* block = decoded.blockAt(localX, y, localZ);
                        if (block != nullptr && block->name != "minecraft:air") {
                            surface = y;
                            break;
                        }
                    }
                    if (surface <= static_cast<int>(kMinY) ||
                        surface >= static_cast<int>(kMinY + kHeight) - 1) {
                        ++clipped;
                        continue;
                    }
                    const double gradient = 1.0 - (2.0 * ((surface + 0.5) - kMinY) / kHeight);
                    out.emplace(std::pair{x, z}, -gradient / (kK * scale));
                }
            }
        }
    }
    return out;
}

/// The island height the inverted field implies: the density function is
/// `(height - 8) / 128`, so this reads back in blocks, which is the unit the
/// field's own constants are written in.
[[nodiscard]] double heightOf(double field) { return (field * 128.0) + 8.0; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: end-islands-analyze <probe dir> <seed> [scan-from scan-to]\n",
                   stderr);
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    const std::int64_t seed = std::strtoll(argv[2], nullptr, 10);

    nlohmann::json manifest;
    {
        std::ifstream in(root / "manifest.json");
        if (!in) {
            // `path::c_str()` is `wchar_t*` on Windows, so a path printed with %s
            // has to go through `.string()`.
            std::fprintf(stderr, "no manifest.json under %s\n", root.string().c_str());
            return 1;
        }
        in >> manifest;
    }
    const auto chunks = manifest.value("chunks", 8);
    // `density-probe.sh` writes the window's origin as a two-element
    // `origin_chunk` array. Read defensively: a manifest from a probe run
    // that predates the key gives the origin, which is what every probe
    // before end_islands used anyway.
    int atChunkX = 0;
    int atChunkZ = 0;
    if (manifest.contains("origin_chunk") && manifest.at("origin_chunk").is_array() &&
        manifest.at("origin_chunk").size() == 2) {
        atChunkX = manifest.at("origin_chunk").at(0).get<int>();
        atChunkZ = manifest.at("origin_chunk").at(1).get<int>();
    }
    const std::string regionName = manifest.value("region", std::string("r.0.0.mca"));
    std::printf("probe %s: seed %lld, %dx%d chunks at chunk (%d, %d), region %s\n",
                root.filename().string().c_str(), static_cast<long long>(seed), chunks, chunks, atChunkX,
                atChunkZ, regionName.c_str());

    const noise::EndIslands islands = noise::EndIslands::fromWorldSeed(seed);

    std::map<std::string, Field> fields;
    for (const Dimension& dimension : kDimensions) {
        const std::filesystem::path region = root / dimension.name / regionName;
        if (!std::filesystem::is_regular_file(region)) {
            std::printf("  %-11s no region at %s\n", dimension.name, region.string().c_str());
            continue;
        }
        std::size_t clipped = 0;
        Field field = readColumns(region, dimension.scale, atChunkX, atChunkZ, chunks, clipped);
        const double quantum = 2.0 / kHeight / (kK * dimension.scale);

        std::size_t agreed = 0;
        double worst = 0.0;
        for (const auto& [position, theirs] : field) {
            const double ours = islands.sample(position.first, position.second);
            const double error = std::abs(ours - theirs);
            worst = std::max(worst, error);
            if (error <= (0.5 * quantum) + 1e-12) {
                ++agreed;
            }
        }
        std::printf("  %-11s legacy=%d scale=%.1f: %zu of %zu columns agree "
                    "(quantum %.5f, worst error %.5f = %.1f blocks of island height), "
                    "%zu clipped\n",
                    dimension.name, static_cast<int>(dimension.legacy), dimension.scale, agreed,
                    field.size(), quantum, worst, worst * 128.0, clipped);
        fields.emplace(dimension.name, std::move(field));
    }

    // Legacy against its control, on the columns both could speak for. If
    // these are identical, `end_islands` ignores the dimension's declared
    // random source.
    for (const char* pair : {"fine", "coarse"}) {
        const std::string legacy = std::string("leg_") + pair;
        const std::string modern = std::string("mod_") + pair;
        if (!fields.contains(legacy) || !fields.contains(modern)) {
            continue;
        }
        std::size_t both = 0;
        std::size_t same = 0;
        for (const auto& [position, value] : fields.at(legacy)) {
            const auto found = fields.at(modern).find(position);
            if (found == fields.at(modern).end()) {
                continue;
            }
            ++both;
            if (std::abs(value - found->second) <= 1e-9) {
                ++same;
            }
        }
        std::printf("  legacy vs modern (%s): %zu of %zu shared columns read the same value\n",
                    pair, same, both);
    }

    // THE SKIP SCAN. `noise::EndIslands` discards 17292 LCG steps between
    // seeding with the world seed and building the simplex, and that number
    // is a measurement with nothing deriving it. So the measurement is kept
    // runnable rather than remembered: pass a range and this rescores the
    // whole neighbourhood, printing every candidate that beats the base rate.
    // What it shows is one spike and no second-best.
    if (argc >= 5 && fields.contains("leg_fine")) {
        const long from = std::strtol(argv[3], nullptr, 10);
        const long to = std::strtol(argv[4], nullptr, 10);
        const Field& field = fields.at("leg_fine");
        const double quantum = 2.0 / kHeight / (kK * 3.0);
        std::printf("\nskip scan over [%ld, %ld], %zu columns, agreement at half a quantum:\n",
                    from, to, field.size());
        std::size_t best = 0;
        long bestSkip = from;
        // Scored in two passes so the range can be wide: a prefix of the
        // columns first, and the rest only for a candidate that survives it.
        // A candidate that misses more than a quarter of the prefix cannot
        // reach the full agreement this is looking for, and the prefix is
        // spread across the window by the map's own key order rather than
        // taken from one corner.
        constexpr std::size_t kPrefix = 64;
        std::size_t survivors = 0;
        for (long skip = from; skip <= to; ++skip) {
            rng::JavaRandom random{seed};
            for (long i = 0; i < skip; ++i) {
                static_cast<void>(random.nextInt());
            }
            const noise::EndIslands candidate{noise::PerlinNoise::fromRandom(random)};
            std::size_t agreed = 0;
            std::size_t seen = 0;
            bool rejected = false;
            for (const auto& [position, theirs] : field) {
                if (std::abs(candidate.sample(position.first, position.second) - theirs) <=
                    (0.5 * quantum) + 1e-12) {
                    ++agreed;
                }
                ++seen;
                if (seen == kPrefix && agreed * 4 < kPrefix * 3) {
                    rejected = true;
                    break;
                }
            }
            if (rejected) {
                continue;
            }
            ++survivors;
            std::printf("  skip %6ld: %zu of %zu\n", skip, agreed, field.size());
            if (agreed > best) {
                best = agreed;
                bestSkip = skip;
            }
        }
        std::printf("  best: skip %ld at %zu of %zu; %zu candidate(s) survived the prefix\n",
                    bestSkip, best, field.size(), survivors);
    }

    // The field itself, beside the prediction, on a coarse grid — one sample
    // every 32 blocks, so a 128-block window prints as 4x4 and a bigger one
    // stays readable. Heights in blocks, which is the unit the outer term's
    // own constants (100, 80, -100, the 9..21 steepness) are written in.
    const auto& reference = fields.contains("leg_fine")  ? fields.at("leg_fine")
                            : fields.contains("leg_coarse") ? fields.at("leg_coarse")
                                                            : Field{};
    if (!reference.empty()) {
        std::printf("\nisland height: SERVER (left) against noise::EndIslands (right), "
                    "every 32 blocks\n");
        const std::int32_t fromX = atChunkX * 16;
        const std::int32_t fromZ = atChunkZ * 16;
        const std::int32_t span = chunks * 16;
        for (std::int32_t z = fromZ; z < fromZ + span; z += 32) {
            for (std::int32_t x = fromX; x < fromX + span; x += 32) {
                const auto found = reference.find({x, z});
                if (found == reference.end()) {
                    std::printf("    ?");
                } else {
                    std::printf("%5.0f", heightOf(found->second));
                }
            }
            std::printf("   |");
            for (std::int32_t x = fromX; x < fromX + span; x += 32) {
                std::printf("%5.0f", static_cast<double>(islands.heightAt(
                                         javamath::floorDiv(x, 8), javamath::floorDiv(z, 8))));
            }
            std::printf("\n");
        }
    }
    return 0;
}
