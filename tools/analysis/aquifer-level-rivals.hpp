// Stratum — rival readings of Q5.3(a) on a constant surface, for the level probe.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The clean-room spec's Q5.3(a): a source centred more than twenty blocks
// above its surface read (qy - 12 > a0, a0 = psl + 8) takes the global
// picker's status — the sea level, the default fluid — before any
// floodedness is weighed. On the ocean branch every such source that did not
// abort is a near-surface sea already, so only a source off it ("land", a
// surface at or above sea_level - 8) can tell readings of the clause apart,
// and each rival here changes only those sources' statuses, and only where
// its reading of the clause and the build's (a margin of 20 over the scan's
// `cap`, which a constant surface makes the anchor and the gate as well)
// disagree:
//
//   gated     no such clause off the ocean branch: the floodedness gates
//             alone (the sea above 0.8, the ladder above 0.4, dry otherwise)
//             — the build before aquifer-level-probe.sh;
//   psl+19    the clause with a margin of 19, and
//   psl+21    of 21;
//   sea+12    the clause keyed on the sea instead: centred more than twelve
//             above sea level — the build's reading where sea = psl + 8, and
//             apart from it where the sea is lower.
//
// `decide` is the substance decision spelled from the library's public
// pieces in `computeSubstance`'s own order, with the statuses supplied. Both
// readers check its library arm against `aquifer::computeSubstance` block by
// block, so it cannot drift from the library unnoticed. Shared by
// tools/analysis/aquifer-level-analyze.cpp and
// tests/conformance/vanilla_aquifer_level_test.cpp, so that the case asserts
// what the analyzer measured, through the same code.
#pragma once

#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/javamath.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>

namespace stratum::analysis::level {

/// One constant-input dimension of the level probe.
struct Dim {
    double psl = 0.0;
    std::int32_t sea = 0;
    double floodedness = 0.0;
    double spread = 0.0;
    double barrier = 0.0;
};

/// A reading of Q5.3(a) off the ocean branch.
struct Rival {
    const char* name = "";
    bool clause = false;  ///< false: the floodedness gates alone
    bool fromSea = false; ///< the threshold's base: the sea, or the surface read
    std::int32_t margin = 0;
};

inline constexpr std::size_t kRivalCount = 4;
inline constexpr std::array<Rival, kRivalCount> kRivals{{
    {.name = "gated", .clause = false, .fromSea = false, .margin = 0},
    {.name = "psl+19", .clause = true, .fromSea = false, .margin = 19},
    {.name = "psl+21", .clause = true, .fromSea = false, .margin = 21},
    {.name = "sea+12", .clause = true, .fromSea = true, .margin = 12},
}};

enum class Verdict : std::uint8_t { Air, Fluid, Lava, Solid };

[[nodiscard]] inline auto constant(const double value) {
    return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
}

/// One source's status — the library's (@p rival null) or a rival's —
/// memoized per centre.
class Statuses {
public:
    Statuses(const Dim& dim, const Rival* rival) : dim_(dim), rival_(rival) {}

    [[nodiscard]] aquifer::SourceStatus of(const aquifer::Source& source) {
        const auto key = std::make_tuple(source.centre.x, source.centre.y, source.centre.z);
        const auto found = memo_.find(key);
        if (found != memo_.end()) {
            return found->second;
        }
        const aquifer::PslRead surface =
            aquifer::readPreliminarySurface(constant(dim_.psl), source.centre, dim_.sea);
        const aquifer::CellFluid cell{.centreY = source.centre.y,
                                      .surface = surface,
                                      .seaLevel = dim_.sea,
                                      .floodedness = dim_.floodedness,
                                      .spread = dim_.spread,
                                      .deepDark = false};
        aquifer::SourceStatus status = aquifer::sourceStatus(cell, 0.0);
        const bool land =
            surface.gate >= javamath::wrappingSub(dim_.sea, aquifer::kOceanGateOffset);
        if (rival_ != nullptr && land && !surface.aborted) {
            // A rival differs from the library only where the two readings
            // of the clause do; everywhere else it IS the library's status,
            // trailing guard and all.
            const bool builds =
                cell.centreY > javamath::wrappingAdd(surface.cap, aquifer::kNearSurfaceFloorOffset);
            const std::int32_t base = rival_->fromSea ? dim_.sea : surface.cap;
            const bool reads =
                rival_->clause && cell.centreY > javamath::wrappingAdd(base, rival_->margin);
            if (reads && !builds) {
                status =
                    aquifer::SourceStatus{.level = dim_.sea, .type = aquifer::FluidType::Default};
            } else if (builds && !reads) {
                status = gated(cell, surface);
            }
        }
        memo_.emplace(key, status);
        return status;
    }

private:
    /// The floodedness gates alone, for a land cell the library sends to the
    /// sea by the clause: centred more than twenty above a cap at or above
    /// lambda - 8, so at or above lambda, where no trailing guard applies.
    [[nodiscard]] aquifer::SourceStatus gated(const aquifer::CellFluid& cell,
                                              const aquifer::PslRead& surface) const {
        std::int32_t level = aquifer::kNeverLevel;
        if (cell.floodedness > aquifer::kFloodedSeaThreshold) {
            level = dim_.sea;
        } else if (cell.floodedness > aquifer::kFloodedLocalThreshold) {
            level = aquifer::ladderLevel(cell.centreY, surface.cap, cell.spread);
        }
        return aquifer::SourceStatus{.level = level,
                                     .type = aquifer::fluidTypeOf(aquifer::FluidTypeAt{
                                         .centreY = cell.centreY,
                                         .level = level,
                                         .seaLevel = dim_.sea,
                                         .lava = 0.0,
                                         .origin = aquifer::LevelOrigin::Cell})};
    }

    const Dim& dim_;
    const Rival* rival_;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, aquifer::SourceStatus> memo_;
};

[[nodiscard]] inline Verdict ofFluid(const aquifer::FluidType type) {
    return type == aquifer::FluidType::Lava ? Verdict::Lava : Verdict::Fluid;
}

/// The global picker, which decides above the chunk's y_skip and below lambda.
[[nodiscard]] inline Verdict global(const Dim& dim, const std::int32_t y) {
    if (aquifer::globalReadsLava(y, dim.sea)) {
        return Verdict::Lava;
    }
    return y < dim.sea ? Verdict::Fluid : Verdict::Air;
}

/// The substance decision at a block the lattice decides (at or below the
/// chunk's y_skip, at or above lambda) whose sources are already ranked, with
/// the statuses @p statuses supplies. Ranking is the costly part, so a reader
/// weighing several readings ranks once.
[[nodiscard]] inline Verdict decideRanked(const aquifer::Selection& selection, const Dim& dim,
                                          Statuses& statuses, const std::int32_t y) {
    std::array<aquifer::SourceStatus, 3> status{};
    for (std::size_t r = 0; r < status.size(); ++r) {
        status.at(r) = statuses.of(selection.ranked.at(r));
    }
    if (aquifer::waterOverLava(y, dim.sea, status[0].level, status[0].type)) {
        return ofFluid(status[0].type);
    }
    const auto source = [&](const std::size_t r) {
        return aquifer::BarrierSource{.level = status.at(r).level,
                                      .distanceSq = selection.ranked.at(r).distanceSq,
                                      .type = status.at(r).type};
    };
    const aquifer::BarrierAt at{.y = y,
                                .density = -1.0,
                                .nearest = source(0),
                                .second = source(1),
                                .third = source(2),
                                .barrier = dim.barrier};
    if (aquifer::placesBarrier(at)) {
        return Verdict::Solid;
    }
    return y < status[0].level ? ofFluid(status[0].type) : Verdict::Air;
}

/// Whether the lattice decides at @p y: at or below y_skip, at or above lambda.
[[nodiscard]] inline bool latticeDecides(const Dim& dim, const std::int32_t ySkipLevel,
                                         const std::int32_t y) {
    return aquifer::consultsLattice(y, ySkipLevel) && !aquifer::globalReadsLava(y, dim.sea);
}

/// The substance decision with the statuses @p statuses supplies.
[[nodiscard]] inline Verdict decide(const aquifer::CentreSource& centres, const Dim& dim,
                                    const std::int32_t ySkipLevel, Statuses& statuses,
                                    const std::int32_t x, const std::int32_t y,
                                    const std::int32_t z) {
    if (!latticeDecides(dim, ySkipLevel, y)) {
        return global(dim, y);
    }
    return decideRanked(aquifer::selectSources(centres, x, y, z), dim, statuses, y);
}

/// The library's own answer: the global picker above y_skip, and
/// `aquifer::computeSubstance` at and below it.
[[nodiscard]] inline Verdict library(const aquifer::CentreSource& centres, const Dim& dim,
                                     const std::int32_t ySkipLevel, aquifer::StatusCache& cache,
                                     const std::int32_t x, const std::int32_t y,
                                     const std::int32_t z) {
    if (!aquifer::consultsLattice(y, ySkipLevel)) {
        return global(dim, y);
    }
    const aquifer::SubstanceAt at = aquifer::computeSubstance(
        centres,
        aquifer::AquiferQuery{.x = x, .y = y, .z = z, .density = -1.0, .seaLevel = dim.sea}, cache,
        constant(dim.barrier), constant(dim.floodedness), constant(dim.spread), constant(0.0),
        constant(dim.psl),
        // Erosion and depth are 0 in a probe: Q5.9 cannot fire.
        aquifer::NoDeepDark{});
    switch (at.substance) {
        case aquifer::Substance::Air:
            return Verdict::Air;
        case aquifer::Substance::Solid:
            return Verdict::Solid;
        case aquifer::Substance::Fluid:
            return ofFluid(at.fluidType);
    }
    return Verdict::Solid;
}

} // namespace stratum::analysis::level
