// Stratum — reads back tools/analysis/aquifer-level-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   stratum_aquifer_level_analyze [--stride N] [--only DIM] [--seeds a,b,c]
//   stratum_aquifer_level_analyze --block CORPUS SEED DIM X Y Z
//
// Replays every block of the levelice_s<seed> corpora at a column stride
// (default 1: every column) against the server, the way
// tests/conformance/vanilla_aquifer_level_test.cpp does at stride 4, and
// beside the library scores the rival readings of Q5.3(a) in
// aquifer-level-rivals.hpp (the floodedness gates alone off the ocean
// branch; the clause at margins 19 and 21; the clause keyed on the sea).
//
// Prints, per dimension with anything to say and per seed: blocks scored
// from y -51 up, the server against the library, a block an ice world cannot
// hold, and "copy drift" — the rivals' shared decision against
// `aquifer::computeSubstance`, which must stay 0; then for each rival the
// blocks where it parts from the library and which of the two the server
// holds there. Rows -54..-52, where lava stands beside packed ice and which
// the conformance case does not assert, are tallied apart by (server, built)
// pair. --block prints one position's verdicts and the server's 3x3x3
// neighbourhood beside the library's, in any corpus of the probe (levelice
// or levelwater). Nothing here asserts; the conformance case does.
#include "aquifer-level-rivals.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace aquifer = stratum::aquifer;
namespace javamath = stratum::javamath;
namespace level = stratum::analysis::level;
using level::Verdict;

constexpr std::int32_t kWindow = 128;
constexpr std::int32_t kEdgeMargin = 2;
constexpr std::int32_t kLambda = -54;
constexpr std::int32_t kFirstAssertedRow = -51;
constexpr std::int32_t kRowsAboveTop = 16;
constexpr std::int32_t kTopRow = 319;
/// The verdicts, then a block no verdict names.
constexpr std::size_t kServerKinds = 5;
constexpr std::size_t kForeign = 4;

const char* nameOf(const std::size_t kind) {
    constexpr std::array<const char*, kServerKinds> kNames{"air", "fluid", "lava", "stone",
                                                           "foreign"};
    return kind < kNames.size() ? kNames.at(kind) : "?";
}

std::size_t kindOf(const Verdict verdict) {
    return static_cast<std::size_t>(verdict);
}

struct NamedDim {
    std::string name;
    level::Dim dim;
};

/// Exact, by bits: the project keeps -Wfloat-equal on.
bool same(const double a, const double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

std::filesystem::path probes() {
    const char* fixtures = std::getenv("STRATUM_FIXTURES_DIR");
    return std::filesystem::path{fixtures != nullptr ? fixtures : ".fixtures"} / "1.21.11" /
           "probes";
}

std::vector<NamedDim> readSpec(const std::filesystem::path& dir) {
    std::ifstream in(dir / "spec.json");
    if (!in) {
        throw std::runtime_error("no spec.json under " + dir.string());
    }
    const nlohmann::json spec = nlohmann::json::parse(in);
    std::vector<NamedDim> dims;
    for (const auto& entry : spec) {
        const nlohmann::json& router = entry.at("router");
        if (!same(router.at("lava").get<double>(), 0.0) ||
            !same(entry.at("raw_final_density").at("argument").get<double>(), -1.0)) {
            throw std::runtime_error("not a level-probe dimension: " +
                                     entry.at("name").get<std::string>());
        }
        dims.push_back(NamedDim{
            .name = entry.at("name").get<std::string>(),
            .dim = level::Dim{.psl = router.at("preliminary_surface_level").get<double>(),
                              .sea = entry.at("sea_level").get<std::int32_t>(),
                              .floodedness = router.at("fluid_level_floodedness").get<double>(),
                              .spread = router.at("fluid_level_spread").get<double>(),
                              .barrier = router.at("barrier").get<double>()}});
    }
    return dims;
}

/// A server block by exact name; the default fluid is packed ice or water.
std::size_t serverKind(const stratum::chunk::BlockState* block) {
    if (block == nullptr) {
        return kForeign;
    }
    const std::string& name = block->name;
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return kindOf(Verdict::Air);
    }
    if (name == "minecraft:packed_ice" || name == "minecraft:water") {
        return kindOf(Verdict::Fluid);
    }
    if (name == "minecraft:lava") {
        return kindOf(Verdict::Lava);
    }
    if (name == "minecraft:stone") {
        return kindOf(Verdict::Solid);
    }
    return kForeign;
}

/// One readable region, decoded a chunk at a time.
class World {
public:
    explicit World(const std::filesystem::path& path)
        : file_(stratum::region::RegionFile::open(path)) {}

    const stratum::chunk::BlockState* blockAt(const std::int32_t x, const std::int32_t y,
                                              const std::int32_t z) {
        const std::int32_t cx = javamath::floorDiv(x, 16);
        const std::int32_t cz = javamath::floorDiv(z, 16);
        const auto key = std::make_pair(cx, cz);
        auto found = chunks_.find(key);
        if (found == chunks_.end()) {
            found = chunks_
                        .emplace(key, stratum::chunk::Chunk::decode(
                                          stratum::nbt::read(file_.readChunk(cx, cz)).root))
                        .first;
        }
        return found->second.blockAt(javamath::floorMod(x, 16), y, javamath::floorMod(z, 16));
    }

private:
    stratum::region::RegionFile file_;
    std::map<std::pair<std::int32_t, std::int32_t>, stratum::chunk::Chunk> chunks_;
};

struct RivalTally {
    long long parted = 0;        ///< blocks where the rival parts from the library
    long long serverLibrary = 0; ///< ...and the server holds the library's block
    long long serverRival = 0;   ///< ...and the server holds the rival's
};

struct Tally {
    long long blocks = 0;
    long long disagree = 0; ///< server against the library, rows -51 up
    long long foreign = 0;  ///< a block an ice world cannot hold, rows -51 up
    long long copyDrift = 0;
    std::array<RivalTally, level::kRivalCount> rivals{};
    /// Rows -54..-52, by (server, built).
    std::array<std::array<long long, kServerKinds>, kServerKinds> lowRows{};

    void add(const Tally& o) {
        blocks += o.blocks;
        disagree += o.disagree;
        foreign += o.foreign;
        copyDrift += o.copyDrift;
        for (std::size_t r = 0; r < rivals.size(); ++r) {
            rivals.at(r).parted += o.rivals.at(r).parted;
            rivals.at(r).serverLibrary += o.rivals.at(r).serverLibrary;
            rivals.at(r).serverRival += o.rivals.at(r).serverRival;
        }
        for (std::size_t s = 0; s < kServerKinds; ++s) {
            for (std::size_t b = 0; b < kServerKinds; ++b) {
                lowRows.at(s).at(b) += o.lowRows.at(s).at(b);
            }
        }
    }

    [[nodiscard]] bool anything() const {
        return disagree != 0 || foreign != 0 || copyDrift != 0 ||
               std::any_of(rivals.begin(), rivals.end(),
                           [](const RivalTally& r) { return r.parted != 0; });
    }
};

std::int32_t topRow(const level::Dim& dim) {
    return std::min(kTopRow, std::max(dim.sea, javamath::floorToInt(dim.psl)) + kRowsAboveTop);
}

Tally scoreDim(const aquifer::CentreSource& centres, const std::filesystem::path& dir,
               const NamedDim& named, const std::int32_t stride) {
    const level::Dim& dim = named.dim;
    World world(dir / named.name / "r.0.0.mca");
    aquifer::StatusCache cache;
    level::Statuses shipped(dim, nullptr);
    std::vector<level::Statuses> rivals;
    rivals.reserve(level::kRivals.size());
    for (const level::Rival& rival : level::kRivals) {
        rivals.emplace_back(dim, &rival);
    }
    const std::int32_t ySkipLevel = aquifer::chunkYSkip(level::constant(dim.psl), 0, 0);
    const std::int32_t top = topRow(dim);
    Tally tally;
    int printed = 0;
    for (std::int32_t z = kEdgeMargin; z < kWindow - kEdgeMargin; z += stride) {
        for (std::int32_t x = kEdgeMargin; x < kWindow - kEdgeMargin; x += stride) {
            for (std::int32_t y = kLambda; y <= top; ++y) {
                const Verdict built = level::library(centres, dim, ySkipLevel, cache, x, y, z);
                const bool lattice = level::latticeDecides(dim, ySkipLevel, y);
                const aquifer::Selection selection =
                    lattice ? aquifer::selectSources(centres, x, y, z) : aquifer::Selection{};
                const auto decide = [&](level::Statuses& statuses) {
                    return lattice ? level::decideRanked(selection, dim, statuses, y)
                                   : level::global(dim, y);
                };
                tally.copyDrift += decide(shipped) != built ? 1 : 0;
                const std::size_t server = serverKind(world.blockAt(x, y, z));
                if (y < kFirstAssertedRow) {
                    ++tally.lowRows.at(server).at(kindOf(built));
                    continue;
                }
                ++tally.blocks;
                if (server == kForeign) {
                    ++tally.foreign;
                    continue;
                }
                if (server != kindOf(built)) {
                    ++tally.disagree;
                    if (printed++ < 4) {
                        std::printf("  %s: (%d, %d, %d) server %s, built %s\n", named.name.c_str(),
                                    x, y, z, nameOf(server), nameOf(kindOf(built)));
                    }
                }
                for (std::size_t r = 0; r < rivals.size(); ++r) {
                    const Verdict other = decide(rivals.at(r));
                    if (other != built) {
                        RivalTally& into = tally.rivals.at(r);
                        ++into.parted;
                        into.serverLibrary += server == kindOf(built) ? 1 : 0;
                        into.serverRival += server == kindOf(other) ? 1 : 0;
                    }
                }
            }
        }
    }
    return tally;
}

void printTally(const std::string& label, const Tally& t) {
    std::printf("%s: %lld blocks from -51, %lld disagree, %lld foreign, copy drift %lld\n",
                label.c_str(), t.blocks, t.disagree, t.foreign, t.copyDrift);
    for (std::size_t r = 0; r < t.rivals.size(); ++r) {
        if (t.rivals.at(r).parted != 0) {
            std::printf("    %-7s parts on %lld: the server holds the library's %lld, the "
                        "rival's %lld\n",
                        level::kRivals.at(r).name, t.rivals.at(r).parted,
                        t.rivals.at(r).serverLibrary, t.rivals.at(r).serverRival);
        }
    }
}

void printLowRows(const Tally& t) {
    std::printf("  rows -54..-52 (server/built):");
    for (std::size_t s = 0; s < kServerKinds; ++s) {
        for (std::size_t b = 0; b < kServerKinds; ++b) {
            if (t.lowRows.at(s).at(b) != 0) {
                std::printf(" %s/%s %lld", nameOf(s), nameOf(b), t.lowRows.at(s).at(b));
            }
        }
    }
    std::printf("\n");
}

void explainBlock(const std::string& corpus, const std::int64_t seed, const std::string& dimName,
                  const std::int32_t x, const std::int32_t y, const std::int32_t z) {
    const std::filesystem::path dir = probes() / corpus;
    const aquifer::CentreSource centres{seed};
    for (const NamedDim& named : readSpec(dir)) {
        if (named.name != dimName) {
            continue;
        }
        const level::Dim& dim = named.dim;
        World world(dir / named.name / "r.0.0.mca");
        const std::int32_t ySkipLevel = aquifer::chunkYSkip(level::constant(dim.psl), 0, 0);
        std::printf("%s at (%d, %d, %d): psl %g sea %d floodedness %g spread %g barrier %g, "
                    "y_skip %d\n",
                    named.name.c_str(), x, y, z, dim.psl, dim.sea, dim.floodedness, dim.spread,
                    dim.barrier, ySkipLevel);
        const aquifer::Selection selection = aquifer::selectSources(centres, x, y, z);
        level::Statuses shipped(dim, nullptr);
        for (std::size_t r = 0; r < 3; ++r) {
            const aquifer::Source& s = selection.ranked.at(r);
            const aquifer::SourceStatus status = shipped.of(s);
            std::printf("  rank %zu: centre (%d, %d, %d), d2 %d, level %d, type %d\n", r + 1,
                        s.centre.x, s.centre.y, s.centre.z, s.distanceSq, status.level,
                        static_cast<int>(status.type));
        }
        const Verdict built = level::decide(centres, dim, ySkipLevel, shipped, x, y, z);
        for (const level::Rival& rival : level::kRivals) {
            level::Statuses statuses(dim, &rival);
            std::printf("  %-7s -> %s\n", rival.name,
                        nameOf(kindOf(level::decide(centres, dim, ySkipLevel, statuses, x, y, z))));
        }
        const auto* block = world.blockAt(x, y, z);
        std::printf("  library %s, server %s\n", nameOf(kindOf(built)),
                    block != nullptr ? block->toString().c_str() : "<none>");
        for (std::int32_t dy = 1; dy >= -1; --dy) {
            std::printf("  y %d (server / library):\n", y + dy);
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                std::printf("   ");
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const auto* near = world.blockAt(x + dx, y + dy, z + dz);
                    const Verdict nearBuilt =
                        level::decide(centres, dim, ySkipLevel, shipped, x + dx, y + dy, z + dz);
                    std::printf(" %-28s/%-6s", near != nullptr ? near->toString().c_str() : "-",
                                nameOf(kindOf(nearBuilt)));
                }
                std::printf("\n");
            }
        }
        return;
    }
    std::printf("no dimension %s\n", dimName.c_str());
}

std::vector<std::int64_t> parseSeeds(const std::string& list) {
    std::vector<std::int64_t> seeds;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = list.find(',', start);
        seeds.push_back(std::stoll(
            list.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        if (comma == std::string::npos) {
            return seeds;
        }
        start = comma + 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        std::int32_t stride = 1;
        std::optional<std::string> only;
        std::vector<std::int64_t> seeds{42, 31337, 8675309};
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args.at(i);
            if (arg == "--stride" && i + 1 < args.size()) {
                stride = std::stoi(args.at(++i));
            } else if (arg == "--only" && i + 1 < args.size()) {
                only = args.at(++i);
            } else if (arg == "--seeds" && i + 1 < args.size()) {
                seeds = parseSeeds(args.at(++i));
            } else if (arg == "--block" && i + 6 < args.size()) {
                explainBlock(args.at(i + 1), std::stoll(args.at(i + 2)), args.at(i + 3),
                             std::stoi(args.at(i + 4)), std::stoi(args.at(i + 5)),
                             std::stoi(args.at(i + 6)));
                return 0;
            } else {
                std::fprintf(
                    stderr, "usage: stratum_aquifer_level_analyze [--stride N] [--only DIM] "
                            "[--seeds a,b]\n"
                            "       stratum_aquifer_level_analyze --block CORPUS SEED DIM X Y Z\n");
                return 2;
            }
        }
        if (stride < 1) {
            std::fprintf(stderr, "--stride must be at least 1\n");
            return 2;
        }
        Tally total;
        for (const std::int64_t seed : seeds) {
            const std::filesystem::path dir = probes() / ("levelice_s" + std::to_string(seed));
            const aquifer::CentreSource centres{seed};
            Tally seedTotal;
            for (const NamedDim& named : readSpec(dir)) {
                if (only.has_value() && named.name != *only) {
                    continue;
                }
                const Tally t = scoreDim(centres, dir, named, stride);
                if (t.anything()) {
                    const level::Dim& d = named.dim;
                    const bool land =
                        javamath::floorToInt(d.psl) >= d.sea - aquifer::kOceanGateOffset;
                    printTally("s" + std::to_string(seed) + " " + named.name + " (psl " +
                                   std::to_string(d.psl) + ", sea " + std::to_string(d.sea) +
                                   ", floodedness " + std::to_string(d.floodedness) + ", barrier " +
                                   std::to_string(d.barrier) + (land ? ", land)" : ", ocean)"),
                               t);
                }
                seedTotal.add(t);
            }
            printTally("seed " + std::to_string(seed), seedTotal);
            printLowRows(seedTotal);
            std::fflush(stdout);
            total.add(seedTotal);
        }
        printTally("total", total);
        printLowRows(total);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
