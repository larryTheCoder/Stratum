// Stratum — the aquifer's level rule where the surface actually varies.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §10 makes this a precondition for the aquifer milestone closing, and
// the reason is a failure this project committed twice. About 1370 probe
// dimensions settled the level rule and every one of them held
// `preliminary_surface_level` at a CONSTANT — under which the rule's four
// surface consumers (the near-surface gate, the depth, the ladder's cap and
// the depth path's own gate) read the same number by construction. A model
// that confuses them scores 1.00000 on such a corpus and says nothing.
//
// THE FIELD IS THREE-VALUED, which is the smallest number that separates an
// aborting prefix-minimum from a point read, and it is READ OUT OF THE WORLD
// rather than reconstructed: each scale ships a companion dimension whose
// terrain height names the arm per column, from the server's own arithmetic.
// The field is constant within every 4x4 quart, which is exactly the lattice
// the scan's anchors and its sixteen-block offsets live on, so the readout
// gives the value at the position the aquifer read.
//
// This corpus is not degenerate, and the case checks that rather than
// assuming it: 94-98% of sources abort the scan, and 72-83% have a prefix
// minimum that differs from the whole-window minimum — the configuration no
// constant-surface dimension can produce at all.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::PslRead;

constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kSpan = kChunks * 16;

/// The three arms, and the readout heights that name them. The heights come
/// from the probe's own `K * flat_cache(indicator) + gradient`, so they are
/// the server's arithmetic rather than a threshold chosen here.
constexpr double kLow = -70.0; ///< below the scan's -62 abort
constexpr double kMid = -20.0; ///< below `sea_level - 8`, so it opens the near-surface path
constexpr double kHigh = 96.0; ///< above it, so it does not

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// The surface field, one value per column, taken from the readout dimension.
class Field {
public:
    explicit Field(const std::filesystem::path& readout) : values_(kSpan * kSpan, 0.0) {
        const auto file = stratum::region::RegionFile::open(readout);
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        std::int32_t top = -65;
                        for (std::int32_t y = 250; y >= -64; --y) {
                            const auto* block = chunk.blockAt(lx, y, lz);
                            if (block != nullptr && block->name != "minecraft:air") {
                                top = y;
                                break;
                            }
                        }
                        const double arm = top < 100 ? kLow : (top < 160 ? kMid : kHigh);
                        values_[index((cx * 16) + lx, (cz * 16) + lz)] = arm;
                    }
                }
            }
        }
    }

    [[nodiscard]] static bool inside(const std::int32_t x, const std::int32_t z) noexcept {
        return x >= 0 && x < kSpan && z >= 0 && z < kSpan;
    }

    [[nodiscard]] double at(const std::int32_t x, const std::int32_t z) const {
        return values_[index(x, z)];
    }

private:
    [[nodiscard]] static std::size_t index(const std::int32_t x, const std::int32_t z) noexcept {
        return static_cast<std::size_t>((z * kSpan) + x);
    }

    std::vector<double> values_;
};

struct Arm {
    const char* readout;
    const char* world;
    double floodedness;
};

/// Three feature sizes crossed with two floodedness values. 0.5 sits between
/// the two gates so the ladder is in play; 0.0 sits below both, so only the
/// ocean branch's reach can flood a source.
constexpr std::array<Arm, 6> kArms{{
    {"r8", "v8f5", 0.5},
    {"r8", "v8f0", 0.0},
    {"r16", "v16f5", 0.5},
    {"r16", "v16f0", 0.0},
    {"r32", "v32f5", 0.5},
    {"r32", "v32f0", 0.0},
}};

struct Score {
    long long blocks = 0;
    long long agree = 0;
    long long sources = 0;
    long long aborted = 0;
    long long prefixDiffersFromWhole = 0;
    std::set<std::int32_t> gateValues;
};

} // namespace

TEST_CASE("the aquifer's level rule holds where the surface varies", "[conformance][aquifer]") {
    Score total;

    for (const Arm& arm : kArms) {
        const std::filesystem::path readout =
            fixtures() / "probes" / "pslvar" / arm.readout / "r.0.0.mca";
        const std::filesystem::path world =
            fixtures() / "probes" / "pslvar" / arm.world / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(readout) ||
            !std::filesystem::is_regular_file(world)) {
            SKIP("no varying-surface aquifer probe at "
                 << world << "; generate it with tools/analysis/aquifer-psl-probe.sh");
        }

        const Field field{readout};
        const CentreSource centres{42};
        const auto file = stratum::region::RegionFile::open(world);
        std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> counted;

        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        // At or above the global lava sea only: below it the
                        // picker overrides the lattice outright.
                        for (std::int32_t y = stratum::aquifer::lambdaLevel(kSeaLevel); y < 200;
                             ++y) {
                            const auto* block = chunk.blockAt(lx, y, lz);
                            if (block == nullptr) {
                                continue;
                            }
                            const bool fluid =
                                block->name == "minecraft:water" || block->name == "minecraft:lava";
                            if (!fluid && block->name != "minecraft:air") {
                                continue; // barrier stone
                            }
                            const std::int32_t x = (cx * 16) + lx;
                            const std::int32_t z = (cz * 16) + lz;
                            const CellIndex centre =
                                stratum::aquifer::selectSources(centres, x, y, z).nearest().centre;

                            // The scan reaches 48 blocks west and 16 east,
                            // north and south. A source whose window leaves
                            // the probe footprint is not measurable here.
                            bool reachable = true;
                            const auto sampler = [&](const std::int32_t sx, std::int32_t,
                                                     const std::int32_t sz) {
                                if (!Field::inside(sx, sz)) {
                                    reachable = false;
                                    return 0.0;
                                }
                                return field.at(sx, sz);
                            };
                            const PslRead read = stratum::aquifer::readPreliminarySurface(
                                sampler, centre, kSeaLevel);
                            if (!reachable) {
                                continue;
                            }

                            const std::int32_t level = stratum::aquifer::cellFluidLevel(
                                stratum::aquifer::CellFluid{.centreY = centre.y,
                                                            .surface = read,
                                                            .seaLevel = kSeaLevel,
                                                            .floodedness = arm.floodedness,
                                                            .spread = 0.0});
                            ++total.blocks;
                            total.agree += static_cast<int>((y < level) == fluid);

                            if (counted.insert({centre.x, centre.y, centre.z}).second) {
                                ++total.sources;
                                total.aborted += static_cast<int>(read.aborted);
                                total.prefixDiffersFromWhole +=
                                    static_cast<int>(read.gate != read.cap);
                                total.gateValues.insert(read.gate);
                            }
                        }
                    }
                }
            }
        }
    }

    REQUIRE(total.blocks > 5000000);
    INFO("blocks " << total.blocks << ", agree " << total.agree << "; sources " << total.sources
                   << ", aborted " << total.aborted << ", prefix != whole "
                   << total.prefixDiffersFromWhole << ", distinct gate values "
                   << total.gateValues.size());

    // The corpus has to be able to say something before the score means
    // anything. All three arms must reach the gate, most sources must abort,
    // and the prefix minimum must differ from the whole-window minimum on a
    // large fraction — none of which a constant surface can produce.
    CHECK(total.gateValues.size() == 3);
    CHECK(total.aborted * 10 > total.sources * 9);
    CHECK(total.prefixDiffersFromWhole * 2 > total.sources);

    // 0.9924, 0.9991 and 0.9968 per arm when this was written. The residual
    // is entirely one-directional — fluid this build calls air, never the
    // reverse — and piles at the global lava sea's top and just under
    // `sea_level`; SPEC §11 names it rather than explaining it.
    CHECK(total.agree * 1000 > total.blocks * 990);
}
