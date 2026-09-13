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
//     blocks it applies to (19 / 176 / 178 once Q6.4's constant is in the
//     predicate), the server on 0 of 3320; one row up the two decisions
//     agree on every block; and a nearest source reading AIR on the same
//     row still gets the server's barriers (93 / 239 / 244), so the
//     asymmetry is real. On the shipped sea (63) the exception's
//     population is EMPTY on three seeds — no source reads water at y = -54
//     at all (its ladder sits at -60 plus a multiple of 3, so it needs a
//     spread of 0.9 or a sea-gated source within five blocks of the sea),
//     and both the server and this build write 0 stone on that row. Real,
//     but in vanilla's own overworld close to unreachable.
//   * Q6.4's mixed-type branch, measured on the same worlds and landed in
//     `placesBarrier` (barrier.hpp's own header has the numbers and the
//     two readings it refuted): every ranked source is TYPED here, whether
//     or not it reads fluid at the block, and a lava body meeting a water
//     body is walled off with a constant. In mixed junctions the server's
//     real barriers missed fall from 1698 to 590 pooled over three seeds,
//     with 0 false stone before and after.
//
// WHAT THE SAME WORLDS LEAVE OPEN is the LEVEL a source carries below
// lambda, not a barrier question: `cellFluidLevel` reports a dry source as
// `lambda` (the spec's `never` is -32512) and clamps a ladder below lambda
// up to it, and re-scoring the same blocks at the spec's levels closes the
// 590 to 0 on the rows above the sea. Named in PROGRESS.md as the next
// slice; it is that function's contract to change.
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

/// One source's status — the clean-room spec's `A = (L, T)`: the level its
/// fluid tops out at (`cellFluidLevel`) and which fluid that is
/// (`fluidTypeOf`). Both are functions of the source's own centre alone.
struct SourceStatus {
    std::int32_t level = 0;
    FluidType type = FluidType::Default;
};

namespace detail {

/// One ranked candidate's own status. The LEVEL is its own
/// `preliminary_surface_level` scan, its own `fluid_level_floodedness` (read
/// at its centre, verbatim, per `floodednessSample`) and its own
/// `fluid_level_spread` (read at CONTRACTED lattice indices, per
/// `spreadSample`); the TYPE is that level plus its own `lava` (read at
/// `lavaSample`'s contracted indices). Three of these are computed per block
/// (the fourth-ranked source never reaches the substance decision —
/// selection.hpp), and every one of the three is TYPED whether or not it
/// reads fluid at the block: Π compares the statuses' types (Q6.4), the same
/// way `placesBarrier` compares their levels.
///
/// ONE PIECE OF Q5.8 IS NOT REPRESENTABLE HERE, and is named rather than
/// hidden: the spec exempts a DRY source (`L = never`) from the lava
/// override, but `cellFluidLevel` reports a dry source as `level = lambda`
/// — indistinguishable from a wet source whose ladder clamped there — so a
/// dry source under a `|lava| > 0.3` cell is typed lava. Unobservable in
/// every world this project has measured Π on (`lava` is a constant 0.0 in
/// all of them); it only ever reaches a decision through Π, and only at a
/// separation Π is marginal at. Carried as a known gap, not guessed at.
template<typename PslSampler, typename FloodednessSampler, typename SpreadSampler,
         typename LavaSampler>
[[nodiscard]] SourceStatus rankedStatusOf(const Source& ranked, const std::int32_t seaLevel,
                                          PslSampler&& psl, FloodednessSampler&& floodedness,
                                          SpreadSampler&& spread, LavaSampler&& lava) {
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
    const std::int32_t level = cellFluidLevel(cell);
    const SamplePos lavaPos = lavaSample(ranked.centre);
    const double lavaValue = lava(lavaPos.x, lavaPos.y, lavaPos.z);
    const FluidType type = fluidTypeOf(FluidTypeAt{
        .centreY = ranked.centre.y, .level = level, .seaLevel = seaLevel, .lava = lavaValue});
    return SourceStatus{.level = level, .type = type};
}

} // namespace detail

/// Memoizes a cell centre's own status across one `fill()` call.
/// `detail::rankedStatusOf`'s expensive part — a `preliminary_surface_level`
/// scan of up to fourteen positions — is the SAME every time the SAME
/// centre wins a rank, and a chunk touches dozens of distinct centres, not
/// thousands of blocks' worth of them: measured, caching here is the
/// difference between minutes and well under a second a chunk (matching the
/// same shape of fix `ChunkFiller::applySurfaceRules`'s own biome cache
/// already uses this file's caller for). The type rides along for the same
/// reason in miniature: it is one more router read per centre, and it is
/// now needed for all three ranked sources on every block rather than for
/// the nearest wet one alone.
class StatusCache {
public:
    template<typename PslSampler, typename FloodednessSampler, typename SpreadSampler,
             typename LavaSampler>
    [[nodiscard]] SourceStatus statusOf(const Source& ranked, const std::int32_t seaLevel,
                                        PslSampler&& psl, FloodednessSampler&& floodedness,
                                        SpreadSampler&& spread, LavaSampler&& lava) {
        const auto key = std::make_tuple(ranked.centre.x, ranked.centre.y, ranked.centre.z);
        const auto found = cache_.find(key);
        if (found != cache_.end()) {
            return found->second;
        }
        const SourceStatus status =
            detail::rankedStatusOf(ranked, seaLevel, psl, floodedness, spread, lava);
        cache_.emplace(key, status);
        return status;
    }

private:
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, SourceStatus> cache_;
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
/// own status — level AND type — then the water-over-lava exception (Q6.3),
/// then the barrier (Q6.2-Q6.6, with Π reading the three types), and finally
/// the nearest source's own reading.
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
/// or seed. See `StatusCache`'s own doc for why one is needed at all.
template<typename BarrierSampler, typename FloodednessSampler, typename SpreadSampler,
         typename LavaSampler, typename PslSampler>
[[nodiscard]] SubstanceAt computeSubstance(const CentreSource& centres, const AquiferQuery& query,
                                           StatusCache& cache, BarrierSampler&& barrier,
                                           FloodednessSampler&& floodedness, SpreadSampler&& spread,
                                           LavaSampler&& lava, PslSampler&& psl) {
    // Q2.4: below the global lava sea the lattice is never consulted — the
    // sea is lava whatever any source says, and it is literal lava, not the
    // dimension's default fluid.
    if (globalReadsLava(query.y, query.seaLevel)) {
        return SubstanceAt{.substance = Substance::Fluid, .fluidType = FluidType::Lava};
    }

    const Selection selection = selectSources(centres, query.x, query.y, query.z);

    // All three statuses, typed unconditionally: Π needs the type of a
    // source that reads AIR at this block as much as of one that reads fluid
    // (barrier.hpp).
    std::array<SourceStatus, 3> status{};
    for (std::size_t r = 0; r < 3; ++r) {
        status[r] =
            cache.statusOf(selection.ranked[r], query.seaLevel, psl, floodedness, spread, lava);
    }

    // Q6.3: water resting on the global lava sea is water, and no barrier
    // is even considered — the `barrier` noise is not read.
    const bool nearestReadsFluid = query.y < status[0].level;
    if (waterOverLava(query.y, query.seaLevel, status[0].level, status[0].type)) {
        return SubstanceAt{.substance = Substance::Fluid, .fluidType = status[0].type};
    }

    const double barrierNoise = barrier(query.x, query.y, query.z);
    const auto asBarrierSource = [&](const std::size_t r) {
        return BarrierSource{.level = status[r].level,
                             .distanceSq = selection.ranked[r].distanceSq,
                             .type = status[r].type};
    };
    const BarrierAt at{
        .y = query.y,
        .density = query.density,
        .nearest = asBarrierSource(0),
        .second = asBarrierSource(1),
        .third = asBarrierSource(2),
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
    return SubstanceAt{.substance = Substance::Fluid, .fluidType = status[0].type};
}

} // namespace stratum::aquifer
