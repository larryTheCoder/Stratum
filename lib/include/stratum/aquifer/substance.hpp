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
// TWO GAPS ARE CARRIED HERE RATHER THAN GUESSED (barrier.hpp's own header
// has the numbers behind both):
//
//   * Q6.3's water-over-lava exception — a water block immediately above the
//     global lava floor should never get a barrier, and this does not check
//     for that, so it MAY place one where the real server would not.
//   * Pi's mixed-fluid-type branch (`= 2.0` when the two competing sources
//     read different fluid types) — `placesBarrier`'s Pi only knows the
//     same-type, level-difference formula, so a junction between a water
//     body and a lava body is decided as though both were the nearest
//     source's own type.
//
// Both are measured to be RARE rather than untouched: every barrier probe
// this project has run holds `lava` at a constant specifically to keep the
// mixed-type question out of scope, and Q6.3 only ever applies to a water
// source sitting directly on the global lava floor. Neither is nothing,
// which is why they are named here instead of silently folded into "the
// aquifer is wired now".
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

/// The full aquifer substance decision for one block (spec Q2.2-Q6.7,
/// clean-room spec/aquifer-spec.md, and SPEC §11's own measurements of each
/// piece): rank the four nearest sources, read each of the three nearest
/// ones' own fluid level, decide the barrier or fall through to the nearest
/// source's own reading, and — where that reading is fluid — its type.
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
    const Selection selection = selectSources(centres, query.x, query.y, query.z);

    std::array<std::int32_t, 3> level{};
    for (std::size_t r = 0; r < 3; ++r) {
        level[r] = cache.levelOf(selection.ranked[r], query.seaLevel, psl, floodedness, spread);
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
    if (query.y >= level[0]) {
        return SubstanceAt{.substance = Substance::Air};
    }

    const CellIndex& nearestCentre = selection.ranked[0].centre;
    const SamplePos lavaPos = lavaSample(nearestCentre);
    const double lavaValue = lava(lavaPos.x, lavaPos.y, lavaPos.z);
    const FluidType type = fluidTypeOf(FluidTypeAt{.centreY = nearestCentre.y,
                                                   .level = level[0],
                                                   .seaLevel = query.seaLevel,
                                                   .lava = lavaValue});
    return SubstanceAt{.substance = Substance::Fluid, .fluidType = type};
}

} // namespace stratum::aquifer
