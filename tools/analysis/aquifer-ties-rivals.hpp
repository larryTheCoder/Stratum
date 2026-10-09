// Stratum — rival readings of how a surface scan decides a source's status.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// One scan of `preliminary_surface_level` decides a source's status through
// the clean-room spec's Q5.3, read as written and now measured
// (`aquifer::cellLevel`): (a) a centre more than twenty above the ANCHOR's
// surface takes the global picker's status at the centre; (b) otherwise the
// FIRST sample in scan order whose adjusted surface is submerged and which
// the centre sits above less four (`qy > p - 4`) decides — A_lava if it is
// below the abort threshold, the sea otherwise; and if no sample fires, the
// level rule runs on the minimum of all thirteen. Pipeline engine v12 read
// four values instead (the prefix minimum, the whole minimum `cap`, the
// anchor, the abort flag): Q5.3(a) on `cap`, an aborted scan's sea only on
// the near-surface path and more than twenty above `cap`, A_lava for every
// other aborted scan. The two part only on these sources:
//
//   AbortAnchor    the anchor aborts, and the centre sits in (cap + 20,
//                  anchor + 20], at or above lambda: v12 gave the sea (its
//                  exemption read `cap`), the spec A_lava (Q5.3(a) reads the
//                  anchor, then the anchor fires).
//   SubmergedFirst a sample that does not abort fires before the abort: the
//                  spec gives the sea, v12 A_lava unless the centre was at or
//                  above lambda and more than twenty above `cap`.
//   LandPrefix     the scan aborts after a land prefix and the centre sits
//                  more than twenty above the anchor: the spec's Q5.3(a)
//                  gives the sea, v12 A_lava.
//   NoneFires      the scan aborts and the centre sits four or more below
//                  every sample, so none fires: the spec runs the level rule
//                  on the whole minimum, v12 gave A_lava.
//   LandAnchor     the scan did not abort, off the ocean branch, and the
//                  centre sits in (cap + 20, anchor + 20]: v12 gave the sea
//                  (Q5.3(a) on `cap`), the spec the level rule.
//
// Each reading here is a status function over a ranked source's inputs and
// its samples, so a reader scores it through `computeSubstanceWith`, the
// decision the filler runs. `firstMatchLevel` spells the spec out sample by
// sample, independently of the library's four-value reduction, and the
// scorer counts every source where the two disagree. Shared by
// tools/analysis/aquifer-ties-analyze.cpp and
// tests/conformance/vanilla_aquifer_ties_test.cpp, so that the case asserts
// what the analyzer measured, through the same code.
#pragma once

#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace stratum::analysis::ties {

/// The anchor, then the window.
inline constexpr std::size_t kSamples = aquifer::kPslWindowSize + 1;

/// One scan's samples, floored, in scan order: the anchor first.
struct Scan {
    std::array<std::int32_t, kSamples> at{};
};

/// The scan `readPreliminarySurface` makes for a source centred at @p centre,
/// sample by sample. Floored as the library floors: since the abort
/// threshold is a whole number, `floor(v) < t` exactly when `v < t`.
template<typename Psl>
[[nodiscard]] Scan scanOf(const Psl& psl, const aquifer::CellIndex centre) {
    const std::int32_t anchorX =
        javamath::floorDiv(centre.x, aquifer::kPslAnchorQuantum) * aquifer::kPslAnchorQuantum;
    const std::int32_t anchorZ =
        javamath::floorDiv(centre.z, aquifer::kPslAnchorQuantum) * aquifer::kPslAnchorQuantum;
    Scan scan;
    scan.at[0] = javamath::floorToInt(psl(anchorX, aquifer::kPreliminarySurfaceSampleY, anchorZ));
    for (std::size_t i = 0; i < aquifer::kPslWindowSize; ++i) {
        const aquifer::PslOffset offset = aquifer::kPslWindow.at(i);
        scan.at[i + 1] = javamath::floorToInt(
            psl(anchorX + offset.dx, aquifer::kPreliminarySurfaceSampleY, anchorZ + offset.dz));
    }
    return scan;
}

/// The abort threshold as the whole number it is.
[[nodiscard]] constexpr std::int32_t abortBelow(const std::int32_t sea) noexcept {
    return javamath::wrappingSub(aquifer::lambdaLevel(sea), aquifer::kOceanGateOffset);
}

/// The index of the first sample below the abort threshold, or kSamples.
[[nodiscard]] constexpr std::size_t firstAbort(const Scan& scan, const std::int32_t sea) noexcept {
    const std::int32_t threshold = abortBelow(sea);
    for (std::size_t i = 0; i < kSamples; ++i) {
        if (scan.at.at(i) < threshold) {
            return i;
        }
    }
    return kSamples;
}

[[nodiscard]] constexpr std::int32_t minimumOf(const Scan& scan) noexcept {
    return *std::ranges::min_element(scan.at);
}

/// Where the clean-room reading and engine v12's part (header).
enum class Case : std::uint8_t {
    Same,
    AbortAnchor,
    SubmergedFirst,
    LandPrefix,
    NoneFires,
    LandAnchor,
    Count,
};

inline constexpr std::array<const char*, static_cast<std::size_t>(Case::Count)> kCaseNames{
    "same", "abort-anchor", "submerged-first", "land-prefix", "none-fires", "land-anchor"};

/// Whether a sample at surface @p p fires spec Q5.3(b) for a centre at @p qy:
/// its adjusted surface (p + 8) is submerged, under the sea, and the centre
/// sits above p - 4 (`qy + 12 > p + 8`).
[[nodiscard]] constexpr bool fires(const std::int32_t p, const std::int32_t qy,
                                   const std::int32_t sea) noexcept {
    return p < javamath::wrappingSub(sea, aquifer::kOceanGateOffset) &&
           qy > javamath::wrappingSub(p, aquifer::kNearSurfaceDepth);
}

[[nodiscard]] constexpr bool aboveAnchor(const Scan& scan, const std::int32_t qy) noexcept {
    return qy > javamath::wrappingAdd(scan.at[0], aquifer::kNearSurfaceFloorOffset);
}

/// Whether a sample before the first abort — one that does not abort —
/// fires (header, SubmergedFirst).
[[nodiscard]] constexpr bool prefixFires(const Scan& scan, const std::int32_t qy,
                                         const std::int32_t sea) noexcept {
    const std::size_t stop = firstAbort(scan, sea);
    for (std::size_t i = 0; i < stop; ++i) {
        if (fires(scan.at.at(i), qy, sea)) {
            return true;
        }
    }
    return false;
}

/// Which of the header's cases a source centred at @p qy over @p scan is in;
/// @p read is the library's reduction of the same scan.
[[nodiscard]] constexpr Case caseOf(const Scan& scan, const aquifer::PslRead& read,
                                    const std::int32_t qy, const std::int32_t sea) noexcept {
    const std::int32_t lambda = aquifer::lambdaLevel(sea);
    const std::int32_t whole = minimumOf(scan);
    const bool overCap = qy > javamath::wrappingAdd(whole, aquifer::kNearSurfaceFloorOffset);
    const bool nearSurface = read.gate < javamath::wrappingSub(sea, aquifer::kOceanGateOffset) &&
                             javamath::wrappingSub(read.gate, qy) < aquifer::kNearSurfaceDepth;
    if (!read.aborted) {
        return !nearSurface && overCap && !aboveAnchor(scan, qy) ? Case::LandAnchor : Case::Same;
    }
    if (firstAbort(scan, sea) == 0) {
        // The anchor aborts: no sample before it can fire.
        if (aboveAnchor(scan, qy)) {
            return Case::Same; // both read the global picker at the centre
        }
        if (fires(scan.at[0], qy, sea) ||
            javamath::wrappingSub(whole, qy) < aquifer::kNearSurfaceDepth) {
            return qy >= lambda && overCap && fires(scan.at[0], qy, sea) ? Case::AbortAnchor
                                                                         : Case::Same;
        }
        return Case::NoneFires;
    }
    if (aboveAnchor(scan, qy)) {
        return nearSurface ? Case::Same : Case::LandPrefix;
    }
    if (prefixFires(scan, qy, sea)) {
        return qy >= lambda && overCap ? Case::Same : Case::SubmergedFirst;
    }
    return javamath::wrappingSub(whole, qy) < aquifer::kNearSurfaceDepth ? Case::Same
                                                                         : Case::NoneFires;
}

/// The level rule below Q5.3's short-circuits, on surface @p surface (Q5.4's
/// minimum) and the anchor's ocean test: `cellLevel`'s own body from the
/// deep-dark override down, spelled out so that engine v12's reading and
/// the sample-by-sample spec can both run it.
[[nodiscard]] inline aquifer::CellLevel levelRule(const aquifer::CellFluid& cell,
                                                  const std::int32_t surface,
                                                  const std::int32_t anchor) noexcept {
    if (cell.deepDark) {
        return aquifer::CellLevel{.level = aquifer::kNeverLevel};
    }
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset);
    const std::int32_t ladder = aquifer::ladderLevel(cell.centreY, surface, cell.spread);
    std::int32_t level = aquifer::kNeverLevel;
    bool tookSea = false;
    if (anchor < oceanGate) {
        const std::int32_t depth = javamath::wrappingSub(surface, cell.centreY);
        const auto reach = static_cast<double>(
            std::max(0, javamath::wrappingSub(aquifer::kZeroBonusDepth, depth)));
        if (cell.floodedness +
                ((reach * aquifer::kSeaBonusNumerator) / aquifer::kSeaBonusDenominator) >
            aquifer::kFloodedSeaThreshold) {
            level = cell.seaLevel;
            tookSea = true;
        } else if (cell.floodedness +
                       ((reach * aquifer::kLocalBonusNumerator) / aquifer::kLocalBonusDenominator) >
                   aquifer::kFloodedLocalThreshold) {
            level = ladder;
        }
    } else if (cell.floodedness > aquifer::kFloodedSeaThreshold) {
        level = cell.seaLevel;
        tookSea = true;
    } else if (cell.floodedness > aquifer::kFloodedLocalThreshold) {
        level = ladder;
    }
    if (cell.centreY < aquifer::lambdaLevel(cell.seaLevel) && tookSea) {
        return aquifer::CellLevel{.level = aquifer::kLavaLevel,
                                  .origin = aquifer::LevelOrigin::GlobalLava};
    }
    return aquifer::CellLevel{.level = level};
}

/// Q1.1's A_lava, as the library carries it.
inline constexpr aquifer::CellLevel kALava{.level = aquifer::kLavaLevel,
                                           .origin = aquifer::LevelOrigin::GlobalLava};

/// The global picker's sea as a Q5.3 short-circuit takes it: the near-surface
/// origin, which types it the default fluid.
[[nodiscard]] constexpr aquifer::CellLevel shortCircuitSea(const std::int32_t sea) noexcept {
    return aquifer::CellLevel{.level = sea, .origin = aquifer::LevelOrigin::NearSurfaceSea};
}

/// The global picker's status at height @p y (spec Q1.2): A_lava below
/// lambda, the sea at or above it.
[[nodiscard]] constexpr aquifer::CellLevel globalAt(const std::int32_t y,
                                                    const std::int32_t sea) noexcept {
    return y < aquifer::lambdaLevel(sea) ? kALava : shortCircuitSea(sea);
}

/// The clean-room spec's Q5.3, read as written, then Q5.4-Q5.6.
[[nodiscard]] inline aquifer::CellLevel firstMatchLevel(const Scan& scan,
                                                        const aquifer::CellFluid& cell) noexcept {
    const std::int32_t qy = cell.centreY;
    const std::int32_t sea = cell.seaLevel;
    if (aboveAnchor(scan, qy)) {
        return globalAt(qy, sea);
    }
    const std::int32_t threshold = abortBelow(sea);
    for (const std::int32_t p : scan.at) {
        if (fires(p, qy, sea)) {
            // The global picker at the adjusted surface p + 8: A_lava below
            // lambda, which is p below the abort threshold.
            return p < threshold ? kALava : shortCircuitSea(sea);
        }
    }
    return levelRule(cell, minimumOf(scan), scan.at[0]);
}

/// Pipeline engine v12's status rule, the reading this probe refuted: the
/// near-surface path on the prefix minimum, an aborted scan's sea there only
/// at or above lambda and more than twenty above @p cap, A_lava for every
/// other aborted scan, and Q5.3(a) on `cap` for a scan that did not abort.
/// @p cap is the exemption's comparand: the scan's `cap` as v12 read it, or
/// a rival's.
[[nodiscard]] inline aquifer::CellLevel v12Level(const aquifer::CellFluid& cell,
                                                 const std::int32_t cap) noexcept {
    const aquifer::PslRead& read = cell.surface;
    const std::int32_t lambda = aquifer::lambdaLevel(cell.seaLevel);
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset);
    const bool overCap =
        cell.centreY > javamath::wrappingAdd(cap, aquifer::kNearSurfaceFloorOffset);
    if (read.gate < oceanGate &&
        javamath::wrappingSub(read.gate, cell.centreY) < aquifer::kNearSurfaceDepth) {
        if (!read.aborted || (cell.centreY >= lambda && overCap)) {
            return shortCircuitSea(cell.seaLevel);
        }
        return kALava;
    }
    if (read.aborted) {
        return kALava;
    }
    if (overCap) {
        return shortCircuitSea(cell.seaLevel);
    }
    return levelRule(cell, read.gate, read.anchor);
}

/// Whether engine v12 sent @p cell down its aborted near-surface path, where
/// its exemption read `cap`.
[[nodiscard]] constexpr bool v12Exempts(const aquifer::CellFluid& cell) noexcept {
    return cell.surface.aborted &&
           cell.surface.gate < javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset) &&
           javamath::wrappingSub(cell.surface.gate, cell.centreY) < aquifer::kNearSurfaceDepth;
}

/// The readings scored.
enum class Reading : std::uint8_t {
    Shipped,        ///< the library: `aquifer::sourceStatus`
    V12,            ///< pipeline engine v12, on every source
    AbortingSample, ///< (a): v12's exemption on the first aborting sample, not `cap`
    // Engine v12's reading on one case's sources only, the library's
    // everywhere else, so that a verdict can be attributed to a case.
    OnAbortAnchor,    ///< (a): v12's exemption on the whole minimum
    OnSubmergedFirst, ///< (c): v12's abort flag over a submerged sample that fires first
    OnLandPrefix,
    OnNoneFires,
    OnLandAnchor, ///< (b): Q5.3(a) on `cap` for a scan that did not abort
    Count,
};

inline constexpr std::size_t kReadingCount = static_cast<std::size_t>(Reading::Count);

inline constexpr std::array<const char*, kReadingCount> kReadingNames{
    "shipped (clean-room Q5.3, first match)",     "engine v12 (cap, the abort flag)",
    "(a) v12's exemption on the aborting sample", "(a) v12 on abort-anchor sources only",
    "(c) v12 on submerged-first sources only",    "    v12 on land-prefix sources only",
    "    v12 on none-fires sources only",         "(b) v12 on land-anchor sources only (cap)"};

/// The case a reading that applies v12 to one case only applies it to.
[[nodiscard]] constexpr Case caseApplied(const Reading reading) noexcept {
    switch (reading) {
        case Reading::OnAbortAnchor:
            return Case::AbortAnchor;
        case Reading::OnSubmergedFirst:
            return Case::SubmergedFirst;
        case Reading::OnLandPrefix:
            return Case::LandPrefix;
        case Reading::OnNoneFires:
            return Case::NoneFires;
        case Reading::OnLandAnchor:
            return Case::LandAnchor;
        case Reading::Shipped:
        case Reading::V12:
        case Reading::AbortingSample:
        case Reading::Count:
            break;
    }
    return Case::Count;
}

[[nodiscard]] constexpr bool sameStatus(const aquifer::SourceStatus& a,
                                        const aquifer::SourceStatus& b) noexcept {
    return a.level == b.level && a.type == b.type;
}

[[nodiscard]] inline aquifer::SourceStatus statusOf(const aquifer::CellLevel& level,
                                                    const aquifer::RankedCell& inputs) noexcept {
    return aquifer::SourceStatus{
        .level = level.level,
        .type = aquifer::fluidTypeOf(aquifer::FluidTypeAt{.centreY = inputs.cell.centreY,
                                                          .level = level.level,
                                                          .seaLevel = inputs.cell.seaLevel,
                                                          .lava = inputs.lava,
                                                          .origin = level.origin})};
}

/// One ranked source as every reading needs it.
struct Source {
    aquifer::RankedCell inputs{};
    Scan scan{};
    Case where = Case::Same;
    /// Whether the scan rebuilt here gives the library's `PslRead`; whether
    /// the sample-by-sample spec gives the library's status; and whether
    /// v12 gives it on a source in no case. All three must hold everywhere,
    /// or the readings are not what they say.
    bool readAgrees = true;
    bool specAgrees = true;
    bool sameAgrees = true;
    /// Each reading's status, by `Reading` (`sourceOf` fills it).
    std::array<aquifer::SourceStatus, kReadingCount> status{};
};

/// One source's status under @p reading.
[[nodiscard]] inline aquifer::SourceStatus statusUnder(const Reading reading,
                                                       const Source& source) noexcept {
    const aquifer::RankedCell& inputs = source.inputs;
    switch (reading) {
        case Reading::Shipped:
            break;
        case Reading::V12:
            return statusOf(v12Level(inputs.cell, inputs.cell.surface.cap), inputs);
        case Reading::AbortingSample:
            if (v12Exempts(inputs.cell)) {
                const std::size_t abortAt = firstAbort(source.scan, inputs.cell.seaLevel);
                return statusOf(v12Level(inputs.cell, source.scan.at.at(abortAt)), inputs);
            }
            break;
        case Reading::OnAbortAnchor:
        case Reading::OnSubmergedFirst:
        case Reading::OnLandPrefix:
        case Reading::OnNoneFires:
        case Reading::OnLandAnchor:
            if (source.where == caseApplied(reading)) {
                return statusOf(v12Level(inputs.cell, inputs.cell.surface.cap), inputs);
            }
            break;
        case Reading::Count:
            break;
    }
    return aquifer::sourceStatus(inputs.cell, inputs.lava);
}

/// A ranked source with every reading's status, computed once.
template<typename Psl>
[[nodiscard]] Source sourceOf(const aquifer::RankedCell& inputs, const Psl& psl,
                              const aquifer::CellIndex centre) {
    Source source{.inputs = inputs, .scan = scanOf(psl, centre)};
    const std::int32_t sea = inputs.cell.seaLevel;
    const std::size_t abortAt = firstAbort(source.scan, sea);
    const bool aborted = abortAt < kSamples;
    std::int32_t prefix = source.scan.at[0];
    for (std::size_t i = 1; i < abortAt; ++i) {
        prefix = std::min(prefix, source.scan.at.at(i));
    }
    const aquifer::PslRead rebuilt{.gate = prefix,
                                   .cap = aborted ? minimumOf(source.scan) : prefix,
                                   .anchor = source.scan.at[0],
                                   .aborted = aborted};
    source.readAgrees = rebuilt == inputs.cell.surface;
    source.where = caseOf(source.scan, inputs.cell.surface, inputs.cell.centreY, sea);
    const aquifer::SourceStatus shipped = aquifer::sourceStatus(inputs.cell, inputs.lava);
    source.specAgrees =
        sameStatus(statusOf(firstMatchLevel(source.scan, inputs.cell), inputs), shipped);
    if (source.where == Case::Same) {
        source.sameAgrees =
            sameStatus(statusOf(v12Level(inputs.cell, inputs.cell.surface.cap), inputs), shipped);
    }
    for (std::size_t r = 0; r < kReadingCount; ++r) {
        source.status.at(r) = statusUnder(static_cast<Reading>(r), source);
    }
    return source;
}

} // namespace stratum::analysis::ties
