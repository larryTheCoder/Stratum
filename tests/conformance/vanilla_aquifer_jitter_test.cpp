// Stratum — the aquifer's cell-centre jitter, scored on the server's blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit vectors for `CentreSource` are self-consistent by construction:
// they pin the algorithm, not its correctness. This is what ties it to
// vanilla, and it does so through an observable with no fitted quantity
// anywhere in it.
//
// In the open-void probe a cell at layer -4 spans y -48..-37 and takes the
// -20 fluid level rather than the lava floor exactly when its centre clears
// y = -40 — that is, when its vertical jitter reaches 8, which for a
// nine-valued draw means it equals 8 exactly. So "is there fluid at y = -42"
// is a one-bit readout of one draw. No threshold, no plane fit, no table.
//
// The one subtlety is which cell a column belongs to. A 16x16 footprint is
// the WRONG answer: horizontal jitter moves a cell's territory by up to nine
// blocks, so edge columns belong to a neighbour. Assigning each column to the
// nearest predicted centre is what takes this from 96% to exact — and it is
// not circular, because the assignment uses the horizontal draws while the
// readout tests the vertical one.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/aquifer/lattice.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int kChunks = 8;
constexpr int kCells = 8;
constexpr int kProbeY = -42;
constexpr int kThresholdLayer = -4;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Tally {
    int wet = 0;
    int total = 0;
};

} // namespace

TEST_CASE("the aquifer's cell centres are the ones the server drew", "[conformance][aquifer]") {
    struct World {
        const char* dir;
        std::int64_t seed;
    };

    static constexpr std::array<World, 4> kWorlds{
        {{"comb_42", 42}, {"comb_7", 7}, {"comb_12345", 12345}, {"comb_999", 999}}};

    int checked = 0;
    int agree = 0;
    int observedPositive = 0;
    int predictedPositive = 0;

    for (const World& world : kWorlds) {
        const std::filesystem::path region = fixtures() / "probes" / world.dir / "jv" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no open-void aquifer probe at "
                 << region << "; generate it with tools/analysis/density-probe.sh");
        }

        const stratum::aquifer::CentreSource centres{world.seed};
        std::vector grid(kCells, std::vector<Tally>(kCells));
        const auto file = stratum::region::RegionFile::open(region);

        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz))
                    continue;
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        const auto* block = chunk.blockAt(lx, kProbeY, lz);
                        if (block == nullptr)
                            continue;
                        const int x = (cx * 16) + lx;
                        const int z = (cz * 16) + lz;

                        // nearest predicted centre among the nine candidates
                        int ownerX = -1;
                        int ownerZ = -1;
                        std::int64_t best = std::int64_t{1} << 60;
                        for (int ox = -1; ox <= 1; ++ox) {
                            for (int oz = -1; oz <= 1; ++oz) {
                                const int qx = cx + ox;
                                const int qz = cz + oz;
                                if (qx < 0 || qx >= kCells || qz < 0 || qz >= kCells)
                                    continue;
                                const auto centre = centres.centreOf(qx, kThresholdLayer, qz);
                                const std::int64_t dx = x - centre.x;
                                const std::int64_t dz = z - centre.z;
                                const std::int64_t distance = (dx * dx) + (dz * dz);
                                if (distance < best) {
                                    best = distance;
                                    ownerX = qx;
                                    ownerZ = qz;
                                }
                            }
                        }
                        if (ownerX < 0)
                            continue;
                        Tally& tally = grid[static_cast<std::size_t>(ownerZ)]
                                           [static_cast<std::size_t>(ownerX)];
                        ++tally.total;
                        tally.wet += static_cast<int>(block->name == "minecraft:water" ||
                                                      block->name == "minecraft:lava");
                    }
                }
            }
        }

        for (int cz = 0; cz < kCells; ++cz) {
            for (int cx = 0; cx < kCells; ++cx) {
                const Tally& tally =
                    grid[static_cast<std::size_t>(cz)][static_cast<std::size_t>(cx)];
                if (tally.total < 120)
                    continue;
                const bool observed = tally.wet * 2 > tally.total;
                const bool predicted = centres.jitterOf(cx, kThresholdLayer, cz).y >=
                                       stratum::aquifer::kJitterBoundY - 1;
                ++checked;
                observedPositive += static_cast<int>(observed);
                predictedPositive += static_cast<int>(predicted);
                agree += static_cast<int>(observed == predicted);
            }
        }
    }

    REQUIRE(checked >= 200);
    INFO("cells " << checked << ", agree " << agree << ", observed positive " << observedPositive
                  << ", predicted positive " << predictedPositive);

    // Every cell, on every seed. A draw this build got wrong would show here:
    // the positives are one ninth of the cells, so guessing cannot reach them.
    CHECK(agree == checked);
    CHECK(predictedPositive == observedPositive);
    CHECK(observedPositive > checked / 20);
}
