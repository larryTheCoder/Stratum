// Stratum — the aquifer's whole answer for one block.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Everything above this file answers one piece of the aquifer's decision in
// isolation: the cell lattice and its fluid level (lattice.hpp), which
// sources compete (selection.hpp), whether they disagree hard enough to
// write a barrier (barrier.hpp), and which fluid a wet source holds
// (fluid_type.hpp). This combines them into the one function a caller
// actually needs (SPEC §10, MA blocker 4).
//
// THE SEA'S TWO CLAUSES ARE LANDED AND MEASURED (`tools/analysis/
// aquifer-waterlava-probe.sh`, three seeds, and the conformance case
// `vanilla_aquifer_waterlava_test.cpp`):
//
//   * Q2.4, the global lava sea. Below `lambda = min(-54, sea_level)` the
//     answer is lava before any source is consulted. Without it the bare
//     fall-through read the nearest source's own type there, which is WATER
//     wherever that source is centred above the sea: wrong on 7682 / 5330 /
//     5736 of 16384 blocks per density at sea 63, on every one of three
//     seeds; with it, 16384 / 16384. (The category tests never saw this —
//     water and lava are both "fluid" to them.)
//   * Q6.3, the water-over-lava exception. On the sea's top row alone —
//     the only row where `Global(y-1)` reads lava and Q2.4 has not already
//     answered — a nearest source reading water is water outright, no
//     barrier. Measured where the row HAS water sources (sea_level -70):
//     the bare logic wrote stone on 14 / 50 / 70 of the 278 / 1102 / 1940
//     blocks it applies to, the server on 0 of 3320; one row up the two
//     decisions agree on every block; and a nearest source reading AIR on
//     the same row still gets the server's barriers (93 / 239 / 244), so
//     the asymmetry is real. On the shipped sea (63) the exception's
//     population is EMPTY on three seeds — no source reads water at y = -54
//     at all (its ladder sits at -60 plus a multiple of 3, so it needs a
//     spread of 0.9 or a sea-gated source within five blocks of the sea),
//     and both the server and this build write 0 stone on that row. Real,
//     but in vanilla's own overworld close to unreachable.
//
// ONE GAP IS STILL CARRIED HERE RATHER THAN GUESSED (barrier.hpp's own
// header has the numbers): Pi's mixed-fluid-type branch (`= 2.0` when the
// two competing sources read different fluid types) — `placesBarrier`'s Pi
// only knows the same-type, level-difference formula, so a junction between
// a water body and a lava body is decided as though both were the nearest
// source's own type. Every barrier probe this project has run holds `lava`
// at a constant specifically to keep that question out of scope; the
// sea_level -70 arm above, where lava-typed sources crowd the rows just
// above the sea, is the first world to show its size — the server writes
// 27-45% more barriers there than this predicate does, all of it in
// junctions with a lava-typed source.
#pragma once

#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>

namespace stratum::aquifer {

/// What the aquifer decided a position holds. The caller substitutes real
/// block states for these: `Solid` is the preset's own `default_block`
/// (Q6.7 — the aquifer never names a block itself), `Fluid` is
/// `default_fluid` or literal lava depending on `SubstanceAt::fluidType`,
/// `Air` is air.
enum class Substance : std::uint8_t { Air, Fluid, Solid };

/// The aquifer's whole answer for one block. `fluidType` is meaningful only
/// when `substance == Substance::Fluid`.
struct SubstanceAt {
    Substance substance = Substance::Air;
    FluidType fluidType = FluidType::Default;
};

/// Everything this decision needs beyond the position and the router reads
/// below. `density` is `D` — the caller's OWN density at this position,
/// before the aquifer touches it (Q2.2): solid is unconditional at `D > 0`,
/// so a caller only reaches this function at all where its own density
/// would otherwise be non-solid.
struct AquiferQuery {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    double density = 0.0;
    std::int32_t seaLevel = 0;
};

namespace detail {

/// One ranked candidate's own fluid level — its own `preliminary_surface_level`
/// scan, its own `fluid_level_floodedness` (read at its centre, verbatim,
/// per `floodednessSample`) and its own `fluid_level_spread` (read at
/// CONTRACTED lattice indices, per `spreadSample`). Three of these are
/// computed per block (the fourth-ranked source never reaches the substance
/// decision — selection.hpp).
template<typename PslSampler, typename FloodednessSampler, typename SpreadSampler>
[[nodiscard]] std::int32_t rankedLevelOf(const Source& ranked, const std::int32_t seaLevel,
                                         PslSampler&& psl, FloodednessSampler&& floodedness,
                                         SpreadSampler&& spread) {
    const PslRead surface = readPreliminarySurface(psl, ranked.centre, seaLevel);
    const SamplePos floodPos = floodednessSample(ranked.centre);
    const double f = floodedness(floodPos.x, floodPos.y, floodPos.z);
    const SamplePos spreadPos = spreadSample(ranked.cell, ranked.centre);
    const double s = spread(spreadPos.x, spreadPos.y, spreadPos.z);
    const CellFluid cell{.centreY = ranked.centre.y,
                         .surface = surface,
                         .seaLevel = seaLevel,
                         .floodedness = f,
                         .spread = s};
    return cellFluidLevel(cell);
}

} // namespace detail

/// Memoizes a cell centre's own fluid level across one `fill()` call.
/// `detail::rankedLevelOf`'s expensive part — a `preliminary_surface_level`
/// scan of up to fourteen positions — is the SAME every time the SAME
/// centre wins a rank, and a chunk touches dozens of distinct centres, not
/// thousands of blocks' worth of them: measured, caching here is the
/// difference between minutes and well under a second a chunk (matching the
/// same shape of fix `ChunkFiller::applySurfaceRules`'s own biome cache
/// already uses this file's caller for).
class LevelCache {
public:
    template<typename PslSampler, typename FloodednessSampler, typename SpreadSampler>
    [[nodiscard]] std::int32_t levelOf(const Source& ranked, const std::int32_t seaLevel,
                                       PslSampler&& psl, FloodednessSampler&& floodedness,
                                       SpreadSampler&& spread) {
        const auto key = std::make_tuple(ranked.centre.x, ranked.centre.y, ranked.centre.z);
        const auto found = cache_.find(key);
        if (found != cache_.end()) {
            return found->second;
        }
        const std::int32_t level =
            detail::rankedLevelOf(ranked, seaLevel, psl, floodedness, spread);
        cache_.emplace(key, level);
        return level;
    }

private:
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::int32_t> cache_;
};

/// Q1.2's global picker, reduced to the one question the substance decision
/// asks of it: does it read LAVA at `y`? The picker is a function of `y`
/// alone — `A_lava = (-54, lava)` below `lambda = min(-54, sea_level)` and
/// the default fluid at or above it — so "the global picker reads lava" is
/// one comparison, with no lattice, no source and no noise behind it. Spelled
/// out because two clauses read it at two DIFFERENT heights: Q2.4 at the
/// block itself, Q6.3 one block below it.
[[nodiscard]] constexpr bool globalReadsLava(const std::int32_t y,
                                             const std::int32_t seaLevel) noexcept {
    return y < lambdaLevel(seaLevel);
}

/// Q6.3, the water-over-lava exception: where the NEAREST source reads water
/// at `y` and the global picker reads lava at `y - 1`, the block is that
/// water outright — no pressure, no barrier noise, whatever the other
/// sources say. Since `globalReadsLava(y - 1)` is `y <= lambda` and Q2.4
/// has already handed every `y < lambda` to the sea, this can fire on
/// EXACTLY ONE ROW per dimension: `y == lambda`, the first row above the
/// lava sea. It is asymmetric on purpose — a nearest source reading AIR
/// on that row gets the ordinary barrier decision (measured: the server
/// writes those barriers).
///
/// "Water" is read as `FluidType::Default`: only a water `default_fluid` has
/// ever been observed in this position (fluid_type.hpp carries the same
/// assumption).
[[nodiscard]] constexpr bool waterOverLava(const std::int32_t y, const std::int32_t seaLevel,
                                           const std::int32_t nearestLevel,
                                           const FluidType nearestType) noexcept {
    const bool nearestReadsWater = y < nearestLevel && nearestType == FluidType::Default;
    return nearestReadsWater && globalReadsLava(y - 1, seaLevel);
}

/// The full aquifer substance decision for one block (spec Q2.2-Q6.7,
/// clean-room spec/aquifer-spec.md, and SPEC §11's own measurements of each
/// piece), in the spec's own order: the global lava sea first (Q2.4), then
/// rank the four nearest sources and read each of the three nearest ones'
/// own fluid level, then the water-over-lava exception (Q6.3), then the
/// barrier (Q6.2-Q6.6), and finally the nearest source's own reading — and,
/// where that reading is fluid, its type.
///
/// Each sampler is called as `double(std::int32_t x, std::int32_t y,
/// std::int32_t z)` at the position its own router entry reads at
/// (sampling.hpp): `barrier` and `floodedness` at true block/point
/// coordinates, `spread`, `lava` and `psl` at the CONTRACTED positions this
/// function itself computes and passes in — a caller supplies the router
/// READ, not the position.
///
/// @p cache belongs to the caller, the same way `CornerCache` does — reused
/// across an entire `fill()` call (or more), never across a different world
/// or seed. See `LevelCache`'s own doc for why one is needed at all.
template<typename BarrierSampler, typename FloodednessSampler, typename SpreadSampler,
         typename LavaSampler, typename PslSampler>
[[nodiscard]] SubstanceAt computeSubstance(const CentreSource& centres, const AquiferQuery& query,
                                           LevelCache& cache, BarrierSampler&& barrier,
                                           FloodednessSampler&& floodedness, SpreadSampler&& spread,
                                           LavaSampler&& lava, PslSampler&& psl) {
    // Q2.4: below the global lava sea the lattice is never consulted — the
    // sea is lava whatever any source says, and it is literal lava, not the
    // dimension's default fluid.
    if (globalReadsLava(query.y, query.seaLevel)) {
        return SubstanceAt{.substance = Substance::Fluid, .fluidType = FluidType::Lava};
    }

    const Selection selection = selectSources(centres, query.x, query.y, query.z);

    std::array<std::int32_t, 3> level{};
    for (std::size_t r = 0; r < 3; ++r) {
        level[r] = cache.levelOf(selection.ranked[r], query.seaLevel, psl, floodedness, spread);
    }

    // The nearest source's own reading at this height, and — only where
    // that reading is fluid — its type. Computed ONCE, here, because Q6.3
    // needs it before the barrier and the fall-through needs it after.
    const bool nearestReadsFluid = query.y < level[0];
    FluidType nearestType = FluidType::Default;
    if (nearestReadsFluid) {
        const CellIndex& nearestCentre = selection.ranked[0].centre;
        const SamplePos lavaPos = lavaSample(nearestCentre);
        const double lavaValue = lava(lavaPos.x, lavaPos.y, lavaPos.z);
        nearestType = fluidTypeOf(FluidTypeAt{.centreY = nearestCentre.y,
                                              .level = level[0],
                                              .seaLevel = query.seaLevel,
                                              .lava = lavaValue});
        // Q6.3: water resting on the global lava sea is water, and no
        // barrier is even considered — the `barrier` noise is not read.
        if (waterOverLava(query.y, query.seaLevel, level[0], nearestType)) {
            return SubstanceAt{.substance = Substance::Fluid, .fluidType = nearestType};
        }
    }

    const double barrierNoise = barrier(query.x, query.y, query.z);
    const BarrierAt at{
        .y = query.y,
        .density = query.density,
        .nearest = BarrierSource{.level = level[0], .distanceSq = selection.ranked[0].distanceSq},
        .second = BarrierSource{.level = level[1], .distanceSq = selection.ranked[1].distanceSq},
        .third = BarrierSource{.level = level[2], .distanceSq = selection.ranked[2].distanceSq},
        .barrier = barrierNoise,
    };
    if (placesBarrier(at)) {
        return SubstanceAt{.substance = Substance::Solid};
    }

    // The barrier has fallen through: the substance is the nearest source's
    // own reading outright (selection.hpp's own conformance case).
    if (!nearestReadsFluid) {
        return SubstanceAt{.substance = Substance::Air};
    }
    return SubstanceAt{.substance = Substance::Fluid, .fluidType = nearestType};
}

} // namespace stratum::aquifer
