// Stratum — the aquifer's source selection, scored on the server's own blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `aquifer_selection_test.cpp` pins the ranking rule against known answers;
// what it cannot do is say the rule is vanilla's. This does, through two
// readouts of the open-void probe that between them need no barrier model at
// all — which matters, because this project's barrier predicate is refuted as
// a general model (SPEC §11) and anything built on it would inherit that.
//
// READOUT ONE — where a barrier is even POSSIBLE. All three terms of the
// spec's Q6.6 carry the nearest pair's similarity `s12` as a factor, so
// `s12 <= 0` short-circuits to the nearest source with no barrier at all
// (Q6.2, confirmed). `s12 = 1 - (d2 - d1)/25`, so every stone block the server
// wrote MUST have `d2 - d1 < 25` under a correct selection. That is a
// one-sided test with no fitted quantity in it, and it is the same
// discriminator that eliminated 264 of the 266 wrong horizontal shifts.
//
// It has teeth. On these four worlds the unshifted cell index puts 3.16% of
// the server's own stone where it says no barrier can exist, and the
// horizontal neighbours (-4,+1,-4) and (-6,+1,-6) put 11 and 19 blocks there.
// The one thing it does NOT separate is `+1` from `+2` vertically — both score
// zero — which is why that component needed three purpose-built probe worlds
// of its own rather than this fixture.
//
// READOUT TWO — what the nearest source actually holds. On a block the server
// left NON-solid, the barrier predicate has fallen through and the substance
// is the nearest source's own reading outright: fluid below its level, air at
// or above it. So `y < cellFluidLevel(rank 1)` is directly observable, and it
// closes over the whole layer at once — the shift, the window, the metric, the
// tie-break, the centre jitter and the level rule, with no fitted quantity
// anywhere in the loop.
//
// The residual is about 5e-5 and it is NOT the selection: a brute-force search
// over a 5x7x5 neighbourhood of cells fixes 0 of it on every seed. Some of it
// sits within sixteen blocks of the probe footprint's edge, but not all — one
// seed's residual is entirely interior — so the cause is left named rather
// than explained. Candidates are the fluid TYPE rule and the `lava` router
// entry, which nobody has measured (SPEC §10, milestone MA blocker 4).
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/lattice.hpp>
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
#include <string>

namespace {

using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::Selection;

/// The `jv` arm of the comb probes: `raw_final_density` a constant -1, so no
/// terrain exists and every solid block in the world belongs to the aquifer.
constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kSurface = 96;
constexpr double kFloodedness = 0.5;
constexpr double kSpread = 0.0;

/// The probe forceloads an 8x8 block of chunks. The region file holds 32x32,
/// and the chunks outside the forceload decode as empty — reading them would
/// score this build against a world the server never generated.
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kMaxY = 200;

struct World {
    const char* dir;
    std::int64_t seed;
};

constexpr std::array<World, 4> kWorlds{
    {{"comb_42", 42}, {"comb_7", 7}, {"comb_12345", 12345}, {"comb_999", 999}}};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// The level a cell holds in this dimension. Every router input is a constant
/// here, so the only variable is the cell's own centre.
[[nodiscard]] std::int32_t levelOf(const CentreSource& centres, const CellIndex cell) {
    const CellIndex centre = centres.centreOf(cell.x, cell.y, cell.z);
    return stratum::aquifer::cellFluidLevel(
        stratum::aquifer::CellFluid{.centreY = centre.y,
                                    .surface = stratum::aquifer::constantSurface(kSurface),
                                    .seaLevel = kSeaLevel,
                                    .floodedness = kFloodedness,
                                    .spread = kSpread});
}

struct Score {
    long long stone = 0;
    long long impossible = 0; ///< stone where our selection says no barrier can form
    long long unshifted = 0;  ///< the same count with the shift of Q3.2 removed
    std::int32_t worstSeparation = -1;
    long long fluidOrAir = 0;
    long long agree = 0;
};

} // namespace

TEST_CASE("the aquifer's competing sources are the ones the server used",
          "[conformance][aquifer]") {
    Score total;

    for (const World& world : kWorlds) {
        const std::filesystem::path region = fixtures() / "probes" / world.dir / "jv" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no open-void aquifer probe at "
                 << region << "; generate it with tools/analysis/density-probe.sh");
        }

        const CentreSource centres{world.seed};
        const auto file = stratum::region::RegionFile::open(region);

        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        for (std::int32_t y = kMinY; y < kMaxY; ++y) {
                            const auto* block = chunk.blockAt(lx, y, lz);
                            if (block == nullptr) {
                                continue;
                            }
                            const std::int32_t x = (cx * 16) + lx;
                            const std::int32_t z = (cz * 16) + lz;
                            const Selection selection =
                                stratum::aquifer::selectSources(centres, x, y, z);

                            if (block->name == "minecraft:stone") {
                                ++total.stone;
                                const std::int32_t separation = selection.separation();
                                total.worstSeparation = separation > total.worstSeparation
                                                            ? separation
                                                            : total.worstSeparation;
                                total.impossible += static_cast<int>(
                                    separation >= stratum::aquifer::kSimilarityRange);

                                // The null control, in the same loop over the
                                // same blocks: the identical rule with the
                                // shift of Q3.2 taken out.
                                const CellIndex bare{.x = stratum::javamath::floorDiv(x, 16),
                                                     .y = stratum::javamath::floorDiv(y, 12),
                                                     .z = stratum::javamath::floorDiv(z, 16)};
                                const auto candidates =
                                    stratum::aquifer::candidatesFor(centres, bare);
                                total.unshifted += static_cast<int>(
                                    stratum::aquifer::rankCandidates(x, y, z, candidates)
                                        .separation() >= stratum::aquifer::kSimilarityRange);
                                continue;
                            }

                            const bool fluid =
                                block->name == "minecraft:water" || block->name == "minecraft:lava";
                            if (!fluid && block->name != "minecraft:air") {
                                // Obsidian, where two bodies meet. The fluid
                                // TYPE rule is unmeasured, so these are not
                                // this test's to predict.
                                continue;
                            }
                            ++total.fluidOrAir;
                            total.agree += static_cast<int>(
                                (y < levelOf(centres, selection.nearest().cell)) == fluid);
                        }
                    }
                }
            }
        }
    }

    REQUIRE(total.stone > 500000);
    REQUIRE(total.fluidOrAir > 10000000);

    INFO("stone " << total.stone << ", impossible " << total.impossible << ", worst separation "
                  << total.worstSeparation << ", unshifted control " << total.unshifted
                  << "; non-solid " << total.fluidOrAir << ", agree " << total.agree);

    // Not one of the server's own barrier blocks may sit where this build says
    // the nearest pair has stopped competing.
    CHECK(total.impossible == 0);

    // And the bound is TIGHT rather than roomy: blocks land on separation 24,
    // one below the clamp, in their thousands. Without this the case above
    // would pass for any rule that overestimates how close two sources are.
    CHECK(total.worstSeparation == stratum::aquifer::kSimilarityRange - 1);

    // The control has to fail, or the case above is measuring nothing.
    CHECK(total.unshifted > 10000);

    // Readout two. 0.99993-0.99996 per seed when this was written; the bound
    // is set well below that so it flags a real regression rather than drift.
    CHECK(total.agree * 10000 > total.fluidOrAir * 9999);
}
