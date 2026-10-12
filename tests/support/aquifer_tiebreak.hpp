// Stratum — counting the aquifer's tie-break where it decides something.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The clean-room spec measures how often the later-wins tie-break of Q4.4
// is visible (Q4.7) by running the post-selection logic twice per tied
// selection — once as ranked, once with the tied pair swapped — and budgets
// golden coverage from those rates. This replays a golden overworld chunk
// the way `ChunkFiller::fill` decides it and does the same, through the
// shipped decision itself (`aquifer::computeSubstanceFrom`), at every
// position where the lattice is consulted: `D <= 0`, at or below `y_skip`,
// not under the global lava sea.
//
// The replay is a mirror of the filler's first pass, so it is checked
// against the filler on every block it sees: the category the mirror
// decides must be the one the filler wrote, and the fluid-update marks
// must be the filler's own, position for position (`Tally::mirror*`). A
// mirror that drifted would count ties in a world the filler does not
// generate.
//
// Shared by tests/conformance/golden_aquifer_tiebreak_test.cpp (a sample,
// pinned) and tools/analysis/aquifer-tiebreak-analyze.cpp (the whole of
// all eight regions, printed). Nothing here is Mojang-derived; the regions
// it reads are (SPEC §12).
#pragma once

#include "support/fluid_flow.hpp"

#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace stratum::test::tiebreak {

/// The three adjacent rank pairs a tie can sit on: ranks 1-2, 2-3 and 3-4.
inline constexpr std::size_t kPairCount = 3;

/// The overworld's lowest final density: its `final_density` is
/// `min(squeeze(...), noodle)`, `squeeze` clamps to [-1, 1] and maps -1 to
/// -1/2 + 1/24, and the noodle branch never goes below -0.1. So `D` at any
/// block the lattice is consulted for lies in [-11/24, 0]. The replay
/// asserts it rather than trusting this paragraph (`Tally::minDensity`).
inline constexpr double kLowestOverworldDensity = -11.0 / 24.0;

/// What the server holds against what the two orders decide, at a block
/// where they decide differently. A golden is a saved region, so fluid may
/// have moved there after generating (support/fluid_flow.hpp): an order
/// "could leave" a golden block when the block is exactly what that order
/// writes — a fluid as a SOURCE, which is how the aquifer places one — or
/// when `explainedByFlow` derives it from what that order writes. Air
/// beside two water sources is the case that settles nothing: the
/// infinite-water rule fills it, so a water source there is what either
/// order could leave.
struct GoldenVerdict {
    /// Only the shipped (later-wins) order could leave it: the server
    /// ranked as this build does.
    std::uint64_t shipped = 0;
    /// Both could.
    std::uint64_t ambiguous = 0;
    /// Only the swapped order could: the server ranked the other way.
    std::uint64_t swapped = 0;
    /// Neither could.
    std::uint64_t neither = 0;

    void absorb(const GoldenVerdict& o) {
        shipped += o.shipped;
        ambiguous += o.ambiguous;
        swapped += o.swapped;
        neither += o.neither;
    }
};

/// One rank pair's ties and what swapping the tied pair does.
struct PairTally {
    /// Selections whose two ranks hold equal squared distances.
    std::uint64_t ties = 0;
    /// ... where the swapped order decides another substance (air, solid,
    /// water, lava), through the shipped decision.
    std::uint64_t substanceChanged = 0;
    /// ... where it sets the fluid-update flag differently (as narrowed:
    /// set only on a fluid result).
    std::uint64_t flagChanged = 0;
    /// ... where `placesBarrier` alone answers differently, whichever exit
    /// the decision takes.
    std::uint64_t barrierChanged = 0;
    /// The golden at each `substanceChanged` block.
    GoldenVerdict golden;

    void absorb(const PairTally& o) {
        ties += o.ties;
        substanceChanged += o.substanceChanged;
        flagChanged += o.flagChanged;
        barrierChanged += o.barrierChanged;
        golden.absorb(o.golden);
    }
};

/// Every rank 2-3 tie, by the condition that rules out a substance change
/// (open question 9). The lemma is in golden_aquifer_tiebreak_test.cpp's
/// header and tests/unit/aquifer_substance_test.cpp; each stage here is the
/// population the next one is drawn from.
struct Funnel23 {
    /// `s12 <= 0`: Q6.2 hands the block to the nearest source, so ranks 2
    /// and 3 reach only the flag.
    std::uint64_t shortCircuit = 0;
    /// Q6.3's exit: the nearest source's water on the lava sea.
    std::uint64_t waterOverLava = 0;
    /// Every other: the barrier is weighed.
    std::uint64_t barrierWeighed = 0;
    /// ... with `d1 == d2` too, so `s12 = s13 = s23 = 1` and the predicate
    /// is symmetric in all three.
    std::uint64_t threeWay = 0;
    /// ... else ranks 2 and 3 carry the same status (level and type).
    std::uint64_t secondEqualsThird = 0;
    /// ... else the nearest shares a status with one of them.
    std::uint64_t nearestShared = 0;
    /// ... else all three statuses differ: the only population the lemma
    /// leaves open.
    std::uint64_t allDistinct = 0;
    /// Of those, the ones where the two orders' predicates agree at EVERY
    /// density: the window is empty.
    std::uint64_t windowEmpty = 0;
    /// ... where they part only at densities below `kLowestOverworldDensity`.
    std::uint64_t windowUnreachable = 0;
    /// ... where some overworld density would part them.
    std::uint64_t windowReachable = 0;
    /// ... and the block's own density does.
    std::uint64_t densityInWindow = 0;
    /// Flag changes off the `shortCircuit` stage. The lemma says none.
    std::uint64_t flagChangedPastShortCircuit = 0;
    /// Substance changes off the `allDistinct` stage, and barrier verdicts
    /// that part where the barrier is weighed but the statuses do not all
    /// differ. The lemma says none of either.
    std::uint64_t substanceChangedOffAllDistinct = 0;
    std::uint64_t barrierChangedOffAllDistinct = 0;

    void absorb(const Funnel23& o) {
        shortCircuit += o.shortCircuit;
        waterOverLava += o.waterOverLava;
        barrierWeighed += o.barrierWeighed;
        threeWay += o.threeWay;
        secondEqualsThird += o.secondEqualsThird;
        nearestShared += o.nearestShared;
        allDistinct += o.allDistinct;
        windowEmpty += o.windowEmpty;
        windowUnreachable += o.windowUnreachable;
        windowReachable += o.windowReachable;
        densityInWindow += o.densityInWindow;
        flagChangedPastShortCircuit += o.flagChangedPastShortCircuit;
        substanceChangedOffAllDistinct += o.substanceChangedOffAllDistinct;
        barrierChangedOffAllDistinct += o.barrierChangedOffAllDistinct;
    }
};

struct Tally {
    std::uint64_t chunks = 0;
    std::uint64_t blocks = 0;
    /// Positions where the lattice is consulted: one selection each.
    std::uint64_t consulted = 0;
    /// ... where two of the twelve candidates' squared distances coincide,
    /// anywhere in the window (spec Q4.5's 15.40%).
    std::uint64_t anyDuplicate = 0;
    std::array<PairTally, kPairCount> pairs{};
    /// The whole strict alternative (earlier candidate holds at every
    /// rank), as spec Q4.6 states its rival: selections whose four ranked
    /// cells differ from the shipped order's, and of those the ones whose
    /// substance differs, with the golden there.
    std::uint64_t strictRankingDiffers = 0;
    std::uint64_t strictSubstanceChanged = 0;
    std::uint64_t strictFlagChanged = 0;
    GoldenVerdict strictGolden;
    Funnel23 funnel;
    /// The mirror against the filler: blocks whose category differs, and
    /// fluid-update marks either side has that the other does not.
    std::uint64_t mirrorCategoryMismatch = 0;
    std::uint64_t mirrorFlagMismatch = 0;
    /// The lowest density at a consulted position.
    double minDensity = std::numeric_limits<double>::infinity();

    void absorb(const Tally& o) {
        chunks += o.chunks;
        blocks += o.blocks;
        consulted += o.consulted;
        anyDuplicate += o.anyDuplicate;
        for (std::size_t p = 0; p < kPairCount; ++p) {
            pairs[p].absorb(o.pairs[p]);
        }
        strictRankingDiffers += o.strictRankingDiffers;
        strictSubstanceChanged += o.strictSubstanceChanged;
        strictFlagChanged += o.strictFlagChanged;
        strictGolden.absorb(o.strictGolden);
        funnel.absorb(o.funnel);
        mirrorCategoryMismatch += o.mirrorCategoryMismatch;
        mirrorFlagMismatch += o.mirrorFlagMismatch;
        minDensity = std::min(minDensity, o.minDensity);
    }
};

/// One tied selection worth looking at: a swap that changed the substance
/// at any rank pair, or a rank 2-3 tie whose three statuses all differ.
struct Event {
    /// 0, 1 or 2: ranks 1-2, 2-3, 3-4.
    std::size_t pair = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    double density = 0.0;
    std::array<std::int32_t, aquifer::kRankCount> distanceSq{};
    std::array<aquifer::SourceStatus, 3> status{};
    Category shipped = Category::Air;
    Category swapped = Category::Air;
    /// The golden's category there, and its fluid `level` (-1 for none).
    Category server = Category::Air;
    int serverLevel = -1;
    /// Rank 2-3, all statuses distinct: the two orders' barrier thresholds
    /// (`barrierThreshold`); NaN otherwise.
    double rankedThreshold = std::numeric_limits<double>::quiet_NaN();
    double swappedThreshold = std::numeric_limits<double>::quiet_NaN();
};

/// The category an aquifer answer writes in a dimension whose default fluid
/// is water (every vanilla preset with aquifers on).
[[nodiscard]] inline Category categoryOf(const aquifer::SubstanceAt& at) {
    switch (at.substance) {
        case aquifer::Substance::Solid:
            return Category::Solid;
        case aquifer::Substance::Fluid:
            return at.fluidType == aquifer::FluidType::Lava ? Category::Lava : Category::Water;
        case aquifer::Substance::Air:
            break;
    }
    return Category::Air;
}

[[nodiscard]] inline bool sameStatus(const aquifer::SourceStatus& a,
                                     const aquifer::SourceStatus& b) {
    return a.level == b.level && a.type == b.type;
}

/// Spec Q4.6's rival ranking: `rankCandidates` with every `<=` made `<`,
/// so that on a tie the EARLIER candidate holds its rank.
[[nodiscard]] inline aquifer::Selection
rankEarlierWins(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                const std::span<const aquifer::Candidate> candidates) {
    aquifer::Selection out{};
    for (const aquifer::Candidate& candidate : candidates) {
        const aquifer::Source source{.cell = candidate.cell,
                                     .centre = candidate.centre,
                                     .distanceSq =
                                         aquifer::squaredDistanceTo(candidate.centre, x, y, z)};
        if (source.distanceSq < out.ranked[0].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = out.ranked[1];
            out.ranked[1] = out.ranked[0];
            out.ranked[0] = source;
        } else if (source.distanceSq < out.ranked[1].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = out.ranked[1];
            out.ranked[1] = source;
        } else if (source.distanceSq < out.ranked[2].distanceSq) {
            out.ranked[3] = out.ranked[2];
            out.ranked[2] = source;
        } else if (source.distanceSq < out.ranked[3].distanceSq) {
            out.ranked[3] = source;
        }
    }
    return out;
}

/// The density at and below which `placesBarrier` answers false: it is
/// `D > T` for one threshold `T`, since each of Q6.6's terms is `D + w*Π > 0`
/// for a `w` and `Π` that do not depend on `D`. Found by bisection on the
/// predicate itself, so nothing here restates Π. Infinite when the
/// predicate is constant over every density in reach.
[[nodiscard]] inline double barrierThreshold(aquifer::BarrierAt at) {
    constexpr double kReach = 1.0e6;
    at.density = -kReach;
    if (aquifer::placesBarrier(at)) {
        return -std::numeric_limits<double>::infinity();
    }
    at.density = kReach;
    if (!aquifer::placesBarrier(at)) {
        return std::numeric_limits<double>::infinity();
    }
    double low = -kReach;
    double high = kReach;
    for (int i = 0; i < 200; ++i) {
        const double mid = low + ((high - low) / 2.0);
        if (mid <= low || mid >= high) {
            break;
        }
        at.density = mid;
        if (aquifer::placesBarrier(at)) {
            high = mid;
        } else {
            low = mid;
        }
    }
    return low;
}

/// The golden's category at one block; air where it holds none.
[[nodiscard]] inline Category serverAt(GoldenRegion& golden, const std::int32_t x,
                                       const std::int32_t y, const std::int32_t z) {
    const chunk::BlockState* theirs = golden.blockAt(x, y, z);
    return theirs != nullptr ? test::categoryOf(theirs->name) : Category::Air;
}

/// Whether an order that writes @p wrote at one block could leave the
/// golden's @p server there (`GoldenVerdict`).
[[nodiscard]] inline bool couldLeave(GoldenRegion& golden, const std::int32_t x,
                                     const std::int32_t y, const std::int32_t z,
                                     const Category server, const Category wrote) {
    if (server == wrote) {
        return !isFluid(server) || fluidLevel(golden.blockAt(x, y, z)) == 0;
    }
    return explainedByFlow(golden, x, y, z, server, wrote);
}

/// The golden's verdict at one block where the shipped order writes
/// @p shipped and the alternative @p alternative.
inline void judge(GoldenRegion& golden, const std::int32_t x, const std::int32_t y,
                  const std::int32_t z, const Category shipped, const Category alternative,
                  GoldenVerdict& verdict) {
    const Category server = serverAt(golden, x, y, z);
    const bool byShipped = couldLeave(golden, x, y, z, server, shipped);
    const bool byAlternative = couldLeave(golden, x, y, z, server, alternative);
    if (byShipped && byAlternative) {
        ++verdict.ambiguous;
    } else if (byShipped) {
        ++verdict.shipped;
    } else if (byAlternative) {
        ++verdict.swapped;
    } else {
        ++verdict.neither;
    }
}

/// One dimension's first pass, replayed through the shipped aquifer.
class Replay {
public:
    Replay(const density::Graph& graph, const density::NoiseRegistry& noises,
           const settings::NoiseSettings& settings)
        : settings_(&settings),
          interpreter_(graph, noises,
                       density::CellGeometry{.width = settings.geometry.cellWidth(),
                                             .height = settings.geometry.cellHeight()}),
          centres_(noises.worldSeed(), noises.source()),
          finalDensity_(settings.router.at(settings::RouterEntry::FinalDensity)),
          barrier_(settings.router.at(settings::RouterEntry::Barrier)),
          floodedness_(settings.router.at(settings::RouterEntry::FluidLevelFloodedness)),
          spread_(settings.router.at(settings::RouterEntry::FluidLevelSpread)),
          lava_(settings.router.at(settings::RouterEntry::Lava)),
          psl_(settings.router.at(settings::RouterEntry::PreliminarySurfaceLevel)),
          erosion_(settings.router.at(settings::RouterEntry::Erosion)),
          depth_(settings.router.at(settings::RouterEntry::Depth)) {}

    /// Where to record `Event`s, or nothing.
    void recordEvents(std::vector<Event>* events) noexcept { events_ = events; }

    /// Replays chunk (@p chunkX, @p chunkZ), which @p filled holds as
    /// `ChunkFiller::fill` (no surface rules) wrote it, into @p tally,
    /// judging every block the two orders decide differently against
    /// @p golden.
    void chunk(const std::int32_t chunkX, const std::int32_t chunkZ,
               const terrain::ChunkBuffer& filled, GoldenRegion& golden, Tally& tally) const {
        const settings::NoiseGeometry& geometry = settings_->geometry;
        const std::int32_t baseX = chunkX * 16;
        const std::int32_t baseZ = chunkZ * 16;
        const std::int32_t seaLevel = settings_->seaLevel;
        density::Interpreter::CornerCache cache(interpreter_.cacheSize());
        aquifer::StatusCache statusCache;
        const density::FlatCacheWindow window =
            terrain::ChunkFiller::flatCacheWindow(chunkX, chunkZ);
        const auto detached = [&](density::NodeIndex node, std::int32_t x, std::int32_t y,
                                  std::int32_t z) {
            return interpreter_.evaluate(node, density::Point{.x = x, .y = y, .z = z}, cache,
                                         window, density::ReadContext::Detached);
        };
        const auto barrierAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return interpreter_.evaluate(barrier_, density::Point{.x = x, .y = y, .z = z}, cache,
                                         window, density::ReadContext::Block);
        };
        const auto floodednessAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return detached(floodedness_, x, y, z);
        };
        const auto spreadAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return detached(spread_, x, y, z);
        };
        const auto lavaAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return detached(lava_, x, y, z);
        };
        const auto pslAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return detached(psl_, x, y, z);
        };
        const auto deepDarkAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
            return aquifer::isDeepDark(detached(erosion_, x, y, z), detached(depth_, x, y, z));
        };
        const auto statusOf = [&](const aquifer::Source& ranked) {
            return statusCache.statusOf(ranked, seaLevel, pslAt, floodednessAt, spreadAt, lavaAt,
                                        deepDarkAt);
        };
        const std::int32_t ySkip = aquifer::chunkYSkip(pslAt, baseX, baseZ);

        std::set<std::tuple<int, std::int32_t, int>> marks;
        for (const terrain::ChunkBuffer::FluidUpdate& mark : filled.fluidUpdates()) {
            marks.emplace(mark.localX, mark.y, mark.localZ);
        }
        std::uint64_t mirroredMarks = 0;

        const std::int32_t cellWidth = geometry.cellWidth();
        const std::int32_t cellHeight = geometry.cellHeight();
        const std::int32_t topY = geometry.minY + geometry.height;
        // The filler's own traversal, cell by cell, so the corner cache
        // holds the way it does there.
        for (std::int32_t cellZ = 0; cellZ < 16; cellZ += cellWidth) {
            for (std::int32_t cellX = 0; cellX < 16; cellX += cellWidth) {
                for (std::int32_t cellBottom = geometry.minY; cellBottom < topY;
                     cellBottom += cellHeight) {
                    const std::int32_t cellTop = std::min(cellBottom + cellHeight, topY);
                    for (std::int32_t y = cellBottom; y < cellTop; ++y) {
                        for (std::int32_t localZ = cellZ; localZ < std::min(cellZ + cellWidth, 16);
                             ++localZ) {
                            for (std::int32_t localX = cellX;
                                 localX < std::min(cellX + cellWidth, 16); ++localX) {
                                const std::int32_t x = baseX + localX;
                                const std::int32_t z = baseZ + localZ;
                                const double density = interpreter_.evaluate(
                                    finalDensity_, density::Point{.x = x, .y = y, .z = z}, cache);
                                const Category wrote =
                                    test::categoryOf(filled.at(localX, y, localZ).name.toString());
                                ++tally.blocks;

                                Category mirrored = Category::Air;
                                bool mirroredMark = false;
                                if (density > 0.0) {
                                    mirrored = Category::Solid;
                                } else if (!aquifer::consultsLattice(y, ySkip) ||
                                           aquifer::globalReadsLava(y, seaLevel)) {
                                    if (y < aquifer::lambdaLevel(seaLevel)) {
                                        mirrored = Category::Lava;
                                    } else if (y < seaLevel) {
                                        mirrored = Category::Water;
                                    }
                                } else {
                                    const aquifer::AquiferQuery query{.x = x,
                                                                      .y = y,
                                                                      .z = z,
                                                                      .density = density,
                                                                      .seaLevel = seaLevel};
                                    const aquifer::SubstanceAt shipped =
                                        consulted(query, statusOf, barrierAt, golden, tally);
                                    mirrored = tiebreak::categoryOf(shipped);
                                    mirroredMark = shipped.fluidUpdate;
                                }
                                tally.mirrorCategoryMismatch +=
                                    static_cast<std::uint64_t>(mirrored != wrote);
                                if (mirroredMark) {
                                    ++mirroredMarks;
                                    tally.mirrorFlagMismatch += static_cast<std::uint64_t>(
                                        marks.count(std::make_tuple(localX, y, localZ)) == 0U);
                                }
                            }
                        }
                    }
                }
            }
        }
        // Every mark the filler wrote, the mirror wrote too.
        if (marks.size() > mirroredMarks) {
            tally.mirrorFlagMismatch += marks.size() - mirroredMarks;
        }
        ++tally.chunks;
    }

private:
    template<typename StatusOf, typename BarrierAtFn>
    aquifer::SubstanceAt consulted(const aquifer::AquiferQuery& query, StatusOf& statusOf,
                                   BarrierAtFn& barrierAt, GoldenRegion& golden,
                                   Tally& tally) const {
        ++tally.consulted;
        tally.minDensity = std::min(tally.minDensity, query.density);
        const std::array<aquifer::Candidate, aquifer::kCandidateCount> candidates =
            aquifer::candidatesFor(centres_, aquifer::cellOf(query.x, query.y, query.z));
        const aquifer::Selection selection =
            aquifer::rankCandidates(query.x, query.y, query.z, candidates);
        const aquifer::SubstanceAt shipped =
            aquifer::computeSubstanceFrom(selection, query, statusOf, barrierAt);
        const Category shippedCategory = tiebreak::categoryOf(shipped);

        std::array<std::int32_t, aquifer::kCandidateCount> distances{};
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            distances[i] =
                aquifer::squaredDistanceTo(candidates[i].centre, query.x, query.y, query.z);
        }
        std::sort(distances.begin(), distances.end());
        tally.anyDuplicate += static_cast<std::uint64_t>(
            std::adjacent_find(distances.begin(), distances.end()) != distances.end());

        // Each tied pair on its own: the spec's Q4.7 swap.
        for (std::size_t p = 0; p < kPairCount; ++p) {
            if (selection.ranked[p].distanceSq != selection.ranked[p + 1].distanceSq) {
                continue;
            }
            PairTally& pair = tally.pairs[p];
            ++pair.ties;
            aquifer::Selection swapped = selection;
            std::swap(swapped.ranked[p], swapped.ranked[p + 1]);
            const aquifer::SubstanceAt alternative =
                aquifer::computeSubstanceFrom(swapped, query, statusOf, barrierAt);
            const Category alternativeCategory = tiebreak::categoryOf(alternative);
            const bool substanceChanged = alternativeCategory != shippedCategory;
            const bool flagChanged = alternative.fluidUpdate != shipped.fluidUpdate;
            pair.substanceChanged += static_cast<std::uint64_t>(substanceChanged);
            pair.flagChanged += static_cast<std::uint64_t>(flagChanged);
            if (substanceChanged) {
                judge(golden, query.x, query.y, query.z, shippedCategory, alternativeCategory,
                      pair.golden);
            }
            Event event{.pair = p,
                        .x = query.x,
                        .y = query.y,
                        .z = query.z,
                        .density = query.density,
                        .distanceSq = {},
                        .status = {},
                        .shipped = shippedCategory,
                        .swapped = alternativeCategory,
                        .server = serverAt(golden, query.x, query.y, query.z),
                        .serverLevel = fluidLevel(golden.blockAt(query.x, query.y, query.z))};
            for (std::size_t r = 0; r < aquifer::kRankCount; ++r) {
                event.distanceSq[r] = selection.ranked[r].distanceSq;
            }
            for (std::size_t r = 0; r < 3; ++r) {
                event.status[r] = statusOf(selection.ranked[r]);
            }

            const aquifer::BarrierAt asRanked =
                barrierInputs(selection, query, statusOf, barrierAt);
            const aquifer::BarrierAt asSwapped = barrierInputs(swapped, query, statusOf, barrierAt);
            const bool barrierChanged =
                aquifer::placesBarrier(asRanked) != aquifer::placesBarrier(asSwapped);
            pair.barrierChanged += static_cast<std::uint64_t>(barrierChanged);
            bool interesting = substanceChanged;
            if (p == 1) {
                interesting = funnel23(selection, query, statusOf, asRanked, asSwapped,
                                       Change{.substance = substanceChanged,
                                              .flag = flagChanged,
                                              .barrier = barrierChanged},
                                       tally.funnel, event) ||
                              interesting;
            }
            if (interesting && events_ != nullptr) {
                events_->push_back(event);
            }
        }

        // The whole strict alternative at once.
        const aquifer::Selection strict = rankEarlierWins(query.x, query.y, query.z, candidates);
        bool differs = false;
        for (std::size_t r = 0; r < aquifer::kRankCount; ++r) {
            differs = differs || !(strict.ranked[r].cell == selection.ranked[r].cell);
        }
        if (differs) {
            ++tally.strictRankingDiffers;
            const aquifer::SubstanceAt alternative =
                aquifer::computeSubstanceFrom(strict, query, statusOf, barrierAt);
            const Category alternativeCategory = tiebreak::categoryOf(alternative);
            tally.strictFlagChanged +=
                static_cast<std::uint64_t>(alternative.fluidUpdate != shipped.fluidUpdate);
            if (alternativeCategory != shippedCategory) {
                ++tally.strictSubstanceChanged;
                judge(golden, query.x, query.y, query.z, shippedCategory, alternativeCategory,
                      tally.strictGolden);
            }
        }
        return shipped;
    }

    /// The barrier's inputs for @p selection, as `computeSubstanceFrom`
    /// builds them.
    template<typename StatusOf, typename BarrierAtFn>
    static aquifer::BarrierAt barrierInputs(const aquifer::Selection& selection,
                                            const aquifer::AquiferQuery& query, StatusOf& statusOf,
                                            BarrierAtFn& barrierAt) {
        const auto source = [&](std::size_t r) {
            const aquifer::SourceStatus status = statusOf(selection.ranked[r]);
            return aquifer::BarrierSource{.level = status.level,
                                          .distanceSq = selection.ranked[r].distanceSq,
                                          .type = status.type};
        };
        return aquifer::BarrierAt{.y = query.y,
                                  .density = query.density,
                                  .nearest = source(0),
                                  .second = source(1),
                                  .third = source(2),
                                  .barrier = barrierAt(query.x, query.y, query.z)};
    }

    /// What swapping one tied pair changed.
    struct Change {
        bool substance = false;
        bool flag = false;
        bool barrier = false;
    };

    /// Files one rank 2-3 tie in @p funnel; true for an all-distinct one,
    /// whose thresholds it writes into @p event.
    template<typename StatusOf>
    static bool funnel23(const aquifer::Selection& selection, const aquifer::AquiferQuery& query,
                         StatusOf& statusOf, const aquifer::BarrierAt& asRanked,
                         const aquifer::BarrierAt& asSwapped, const Change change, Funnel23& funnel,
                         Event& event) {
        const aquifer::SourceStatus a1 = statusOf(selection.ranked[0]);
        const aquifer::SourceStatus a2 = statusOf(selection.ranked[1]);
        const aquifer::SourceStatus a3 = statusOf(selection.ranked[2]);
        const auto offAllDistinct = [&] {
            funnel.substanceChangedOffAllDistinct += static_cast<std::uint64_t>(change.substance);
        };
        if (aquifer::similarity(selection.ranked[0].distanceSq, selection.ranked[1].distanceSq) <=
            0.0) {
            ++funnel.shortCircuit;
            offAllDistinct();
            return false;
        }
        funnel.flagChangedPastShortCircuit += static_cast<std::uint64_t>(change.flag);
        if (aquifer::waterOverLava(query.y, query.seaLevel, a1.level, a1.type)) {
            ++funnel.waterOverLava;
            offAllDistinct();
            return false;
        }
        ++funnel.barrierWeighed;
        const bool threeWay = selection.ranked[0].distanceSq == selection.ranked[1].distanceSq;
        const bool secondEqualsThird = !threeWay && sameStatus(a2, a3);
        const bool nearestShared =
            !threeWay && !secondEqualsThird && (sameStatus(a1, a2) || sameStatus(a1, a3));
        if (threeWay || secondEqualsThird || nearestShared) {
            funnel.threeWay += static_cast<std::uint64_t>(threeWay);
            funnel.secondEqualsThird += static_cast<std::uint64_t>(secondEqualsThird);
            funnel.nearestShared += static_cast<std::uint64_t>(nearestShared);
            funnel.barrierChangedOffAllDistinct += static_cast<std::uint64_t>(change.barrier);
            offAllDistinct();
            return false;
        }
        ++funnel.allDistinct;
        const double ranked = barrierThreshold(asRanked);
        const double swapped = barrierThreshold(asSwapped);
        event.rankedThreshold = ranked;
        event.swappedThreshold = swapped;
        // The two predicates part on densities in (low, high].
        const double low = std::min(ranked, swapped);
        const double high = std::max(ranked, swapped);
        if (!(low < high)) {
            ++funnel.windowEmpty;
        } else if (high < kLowestOverworldDensity || low >= 0.0) {
            ++funnel.windowUnreachable;
        } else {
            ++funnel.windowReachable;
        }
        funnel.densityInWindow += static_cast<std::uint64_t>(change.substance);
        return true;
    }

    const settings::NoiseSettings* settings_;
    std::vector<Event>* events_ = nullptr;
    density::Interpreter interpreter_;
    aquifer::CentreSource centres_;
    density::NodeIndex finalDensity_;
    density::NodeIndex barrier_;
    density::NodeIndex floodedness_;
    density::NodeIndex spread_;
    density::NodeIndex lava_;
    density::NodeIndex psl_;
    density::NodeIndex erosion_;
    density::NodeIndex depth_;
};

} // namespace stratum::test::tiebreak
