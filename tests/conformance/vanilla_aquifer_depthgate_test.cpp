// Stratum — which surface reading gates the aquifer's depth path.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `cellFluidLevel`'s near-surface path gates on `gate`, the scan's prefix
// minimum. The depth path gates on a DIFFERENT field, `anchor`, the single
// sample at the cell's own quart position. SPEC §10 flagged that asymmetry
// as single-sourced: one agent, one instrument family, and it is exactly the
// shape — one path reading a minimum, a neighbouring path reading a single
// sample — that has been wrong twice before in this codebase.
//
// THE FIELD. Two large psl regions, HIGH (100) and LOW (40), sized around
// 100 blocks — comfortably larger than the scan window's own reach (48
// blocks west, 16 the other three directions). Near every region boundary
// there are cells whose ANCHOR sample lands in the HIGH region while at
// least one WINDOW OFFSET reaches into the LOW one, pulling `gate` down to
// 40 while `anchor` stays at 100 — a separation of 60, eight times the
// margin blocker 1 asks for. A companion readout dimension observes the
// field directly rather than reconstructing it.
//
// THE READOUT. A cell's own geometric centre is always its own nearest
// source (distance zero), and lies strictly inside its ~12-block vertical
// territory — unlike a wide search band, which mostly belongs to OTHER
// cells and produced a first, wrong version of this analysis. Checking the
// exact block at a cell's own centre against both hypotheses' predictions
// removes that confound entirely.
//
// Nine floodedness values comb 0.05 through 0.85; each cell's own measured
// `anchor`, `gate` and centreY, not a hand-picked case, determine what the
// two hypotheses predict, so this covers whatever reach values the region
// boundary actually produces rather than one lucky configuration.
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

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

using stratum::aquifer::CellFluid;
using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::PslRead;

constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kSpan = 128;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// The HIGH/LOW psl field, read off the readout dimension.
class Field {
public:
    explicit Field(const std::filesystem::path& readout) : values_(kSpan * kSpan, 0.0) {
        const auto file = stratum::region::RegionFile::open(readout);
        for (std::int32_t cz = 0; cz < 8; ++cz) {
            for (std::int32_t cx = 0; cx < 8; ++cx) {
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
                        values_[index((cx * 16) + lx, (cz * 16) + lz)] = top < 127 ? 40.0 : 100.0;
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

/// Predict a cell's level under one of the two candidate gating rules,
/// reusing the library's own constants and helpers throughout so a mismatch
/// here would be a bug in this test, not a re-derivation risk.
[[nodiscard]] std::int32_t predictLevel(const CellFluid& cell, const bool gateGated) {
    const std::int32_t lambda = stratum::aquifer::lambdaLevel(cell.seaLevel);
    const std::int32_t ladder =
        stratum::aquifer::ladderLevel(cell.centreY, cell.surface.cap, cell.spread, cell.seaLevel);
    const std::int32_t oceanGate = cell.seaLevel - stratum::aquifer::kOceanGateOffset;
    const bool onDepthPath =
        gateGated ? (cell.surface.gate < oceanGate) : (cell.surface.anchor < oceanGate);
    if (onDepthPath) {
        const std::int32_t depth = cell.surface.gate - cell.centreY;
        const auto reach =
            static_cast<double>(std::max(0, stratum::aquifer::kZeroBonusDepth - depth));
        if (!cell.surface.aborted &&
            cell.floodedness + ((reach * stratum::aquifer::kSeaBonusNumerator) /
                                stratum::aquifer::kSeaBonusDenominator) >
                stratum::aquifer::kFloodedSeaThreshold) {
            return cell.seaLevel;
        }
        if (cell.floodedness + ((reach * stratum::aquifer::kLocalBonusNumerator) /
                                stratum::aquifer::kLocalBonusDenominator) >
            stratum::aquifer::kFloodedLocalThreshold) {
            return ladder;
        }
        return lambda;
    }
    if (!cell.surface.aborted && cell.floodedness > stratum::aquifer::kFloodedSeaThreshold) {
        return cell.seaLevel;
    }
    if (cell.floodedness > stratum::aquifer::kFloodedLocalThreshold) {
        return ladder;
    }
    return lambda;
}

constexpr std::array<double, 9> kFloodednessComb{
    {0.05, 0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85}};

struct Score {
    long long discriminating = 0;
    long long matchesGate = 0;
    long long matchesAnchor = 0;
    long long matchesNeither = 0;
};

} // namespace

TEST_CASE("the aquifer's depth path is gated by the anchor, not the window minimum",
          "[conformance][aquifer]") {
    const std::filesystem::path readout = fixtures() / "probes" / "depthgate" / "r" / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(readout)) {
        SKIP("no depth-gate aquifer probe at "
             << readout << "; generate it with tools/analysis/aquifer-depthgate-probe.sh");
    }
    const Field field{readout};
    const CentreSource centres{42};

    // Enumerate the candidate cell indices once; the field and geometry are
    // shared across every floodedness dimension.
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> cellIndices;
    for (std::int32_t x = 0; x < kSpan; x += 2) {
        for (std::int32_t z = 0; z < kSpan; z += 2) {
            for (std::int32_t y = -20; y < 45; y += 2) {
                const CellIndex c =
                    stratum::aquifer::selectSources(centres, x, y, z).nearest().cell;
                cellIndices.insert({c.x, c.y, c.z});
            }
        }
    }

    Score total;
    for (const double flood : kFloodednessComb) {
        const auto label = std::to_string(static_cast<int>(std::lround(flood * 100)));
        const std::filesystem::path world =
            fixtures() / "probes" / "depthgate" / ("f" + label) / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(world)) {
            SKIP("no depth-gate aquifer probe at "
                 << world << "; generate it with tools/analysis/aquifer-depthgate-probe.sh");
        }
        const auto file = stratum::region::RegionFile::open(world);

        for (const auto& [ix, iy, iz] : cellIndices) {
            const CellIndex centre = centres.centreOf(ix, iy, iz);

            bool reachable = true;
            const auto sampler = [&](const std::int32_t sx, std::int32_t, const std::int32_t sz) {
                if (!Field::inside(sx, sz)) {
                    reachable = false;
                    return 0.0;
                }
                return field.at(sx, sz);
            };
            const PslRead surface =
                stratum::aquifer::readPreliminarySurface(sampler, centre, kSeaLevel);
            if (!reachable) {
                continue;
            }

            const std::int32_t oceanGate = kSeaLevel - stratum::aquifer::kOceanGateOffset;
            if (surface.anchor < oceanGate) {
                continue; // anchor already low: not discriminating
            }
            if (surface.gate >= oceanGate) {
                continue; // gate not low: not discriminating
            }
            if (surface.anchor - surface.gate <= 8) {
                continue; // margin required by blocker 1
            }
            if (surface.gate - centre.y < stratum::aquifer::kNearSurfaceDepth) {
                continue; // the near-surface path would intercept first
            }

            const CellFluid cell{.centreY = centre.y,
                                 .surface = surface,
                                 .seaLevel = kSeaLevel,
                                 .floodedness = flood,
                                 .spread = 0.0};
            const std::int32_t gateLevel = predictLevel(cell, true);
            const std::int32_t anchorLevel = predictLevel(cell, false);
            const bool predictedFluidGate = centre.y < gateLevel;
            const bool predictedFluidAnchor = centre.y < anchorLevel;
            if (predictedFluidGate == predictedFluidAnchor) {
                continue; // not discriminating AT the cell's own centre
            }

            // The cell's own centre is always its own nearest source; check
            // the exact block there, strictly inside its ~12-block territory.
            if (!(stratum::aquifer::selectSources(centres, centre.x, centre.y, centre.z)
                      .nearest()
                      .centre == centre)) {
                continue;
            }
            const std::int32_t chunkX = stratum::javamath::floorDiv(centre.x, 16);
            const std::int32_t chunkZ = stratum::javamath::floorDiv(centre.z, 16);
            if (chunkX < 0 || chunkX >= 8 || chunkZ < 0 || chunkZ >= 8 ||
                !file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const std::int32_t localX = centre.x >= 0 ? centre.x % 16 : ((centre.x % 16) + 16) % 16;
            const std::int32_t localZ = centre.z >= 0 ? centre.z % 16 : ((centre.z % 16) + 16) % 16;
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            const auto* block = chunk.blockAt(localX, centre.y, localZ);
            if (block == nullptr) {
                continue;
            }
            const bool observedFluid =
                block->name == "minecraft:water" || block->name == "minecraft:lava";
            if (!observedFluid && block->name != "minecraft:air") {
                continue; // barrier stone
            }

            ++total.discriminating;
            const bool matchesGate = predictedFluidGate == observedFluid;
            const bool matchesAnchor = predictedFluidAnchor == observedFluid;
            total.matchesGate += static_cast<int>(matchesGate);
            total.matchesAnchor += static_cast<int>(matchesAnchor);
            total.matchesNeither += static_cast<int>(!matchesGate && !matchesAnchor);
        }
    }

    REQUIRE(total.discriminating > 150);
    INFO("discriminating " << total.discriminating << ", matches gate-hyp " << total.matchesGate
                           << ", matches anchor-hyp " << total.matchesAnchor << ", matches neither "
                           << total.matchesNeither);

    // 202/210 (96.2%) when this was written; the eight exceptions are two
    // specific cells whose ladder level exactly equals their own centreY, a
    // boundary tie in the readout rather than a rival pattern.
    CHECK(total.matchesAnchor * 10 > total.discriminating * 9);
    // And the rival hypothesis has to actually lose, not merely not-win.
    CHECK(total.matchesGate * 4 < total.discriminating);
}
