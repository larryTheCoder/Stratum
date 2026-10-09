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
// The last two cases settle WHERE the aquifer reads that surface. The surface
// rule reads the same router entry through a 16-block lattice; the aquifer
// reads it per column. Scored head to head on the blocks where the two
// readings disagree — here and on the barrier-on nsfloor corpus at three
// seeds — the server sides with the lattice on none.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/terrain/filler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::PslRead;
using stratum::terrain::ChunkFiller;

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

/// The readout is exact only where `flat_cache` pins it: the corners of the
/// 4x4 quarts. Every read below lands on one — the scan's anchor is a
/// multiple of four and its offsets multiples of sixteen, and the 16-block
/// lattice's corners are multiples of sixteen.
constexpr std::int32_t kReadoutQuantum = 4;

/// The surface field, one value per column, taken from the readout dimension
/// over its first @p chunks chunks on each axis. The aquifer's footprint is
/// the forceloaded 8x8; the rows and columns past it are the server's border,
/// which the region also holds, and they carry the 16-block lattice's far
/// corners (x or z = 128, and 144 for `y_skip`'s rectangle).
class Field {
public:
    explicit Field(const std::filesystem::path& readout, const std::int32_t chunks = kChunks)
        : span_(chunks * 16),
          values_(static_cast<std::size_t>(span_) * static_cast<std::size_t>(span_), 0.0) {
        const auto file = stratum::region::RegionFile::open(readout);
        for (std::int32_t cz = 0; cz < chunks; ++cz) {
            for (std::int32_t cx = 0; cx < chunks; ++cx) {
                INFO("readout chunk (" << cx << ", " << cz << ") of " << readout);
                REQUIRE(file.hasChunk(cx, cz));
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                // A column with no terrain at all is a chunk the server never
                // took to the noise step, not a low arm: fail, never guess.
                std::int32_t empty = 0;
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
                        empty += static_cast<std::int32_t>(top == -65);
                        const double arm = top < 100 ? kLow : (top < 160 ? kMid : kHigh);
                        values_[index((cx * 16) + lx, (cz * 16) + lz)] = arm;
                    }
                }
                REQUIRE(empty == 0);
            }
        }
    }

    /// Whether (@p x, @p z) is in the aquifer worlds' forceloaded footprint.
    [[nodiscard]] static bool inside(const std::int32_t x, const std::int32_t z) noexcept {
        return x >= 0 && x < kSpan && z >= 0 && z < kSpan;
    }

    /// Whether this field read column (@p x, @p z).
    [[nodiscard]] bool covers(const std::int32_t x, const std::int32_t z) const noexcept {
        return x >= 0 && x < span_ && z >= 0 && z < span_;
    }

    /// The arm at (@p x, @p z), which must be a quart corner this field read.
    [[nodiscard]] double at(const std::int32_t x, const std::int32_t z) const {
        if (!covers(x, z) || stratum::javamath::floorMod(x, kReadoutQuantum) != 0 ||
            stratum::javamath::floorMod(z, kReadoutQuantum) != 0) {
            throw std::out_of_range("the readout is not exact at (" + std::to_string(x) + ", " +
                                    std::to_string(z) + ")");
        }
        return values_[index(x, z)];
    }

private:
    [[nodiscard]] std::size_t index(const std::int32_t x, const std::int32_t z) const noexcept {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(span_)) +
               static_cast<std::size_t>(x);
    }

    std::int32_t span_;
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
    /// Disagreements that are fluid the server's ticks moved (fluid_flow.hpp).
    long long flow = 0;
    /// And the rest, by direction.
    long long serverFluidOursAir = 0;
    long long serverAirOursFluid = 0;
    long long sources = 0;
    long long aborted = 0;
    long long prefixDiffersFromWhole = 0;
    std::set<std::int32_t> gateValues;
};

/// Scores one aquifer world against the level rule, the surface read through
/// @p surfaceAt(x, z) — the readout field for a varying arm, a constant for a
/// control. One path for both, so a control that fails is the harness
/// failing, not a second implementation of it.
template<typename SurfaceAt>
void scoreWorld(const std::filesystem::path& world, SurfaceAt&& surfaceAt, double floodedness,
                Score& total) {
    const CentreSource centres{42, stratum::density::RandomSource::Xoroshiro};
    const auto file = stratum::region::RegionFile::open(world);
    stratum::test::GoldenRegion golden(world);
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> counted;

    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            // A probe region holds every chunk of its window: a missing one is
            // a broken corpus, not a smaller sample.
            REQUIRE(file.hasChunk(cx, cz));
            const auto chunk =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    // At or above the global lava sea only: below it the
                    // picker overrides the lattice outright.
                    for (std::int32_t y = stratum::aquifer::lambdaLevel(kSeaLevel); y < 200; ++y) {
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
                            return surfaceAt(sx, sz);
                        };
                        const PslRead read =
                            stratum::aquifer::readPreliminarySurface(sampler, centre, kSeaLevel);
                        if (!reachable) {
                            continue;
                        }

                        const std::int32_t level = stratum::aquifer::cellFluidLevel(
                            stratum::aquifer::CellFluid{.centreY = centre.y,
                                                        .surface = read,
                                                        .seaLevel = kSeaLevel,
                                                        .floodedness = floodedness,
                                                        .spread = 0.0});
                        ++total.blocks;
                        const bool oursFluid = y < level;
                        if (oursFluid == fluid) {
                            ++total.agree;
                        } else if (fluid &&
                                   stratum::test::explainedByFlow(
                                       golden, x, y, z, stratum::test::categoryOf(block->name),
                                       stratum::test::Category::Air)) {
                            ++total.flow;
                        } else {
                            ++(fluid ? total.serverFluidOursAir : total.serverAirOursFluid);
                        }

                        if (counted.insert({centre.x, centre.y, centre.z}).second) {
                            ++total.sources;
                            total.aborted += static_cast<int>(read.aborted);
                            total.prefixDiffersFromWhole += static_cast<int>(read.gate != read.cap);
                            total.gateValues.insert(read.gate);
                        }
                    }
                }
            }
        }
    }
}

// --- Per column, or through the surface rule's 16-block lattice ------------

constexpr std::int32_t kLatticePitch = ChunkFiller::kPreliminarySurfacePitch;

/// The value the SURFACE RULE gets for column (@p x, @p z): the engine's own
/// `preliminarySurfaceIn`, fed the four lattice corners around it — the same
/// call `vanilla_psl_lattice_test.cpp` scores against the server, so a change
/// to the engine this file does not follow shows up here as a failure.
[[nodiscard]] std::int32_t latticeSurface(const Field& field, const std::int32_t x,
                                          const std::int32_t z) {
    const std::int32_t x0 = stratum::javamath::floorDiv(x, kLatticePitch) * kLatticePitch;
    const std::int32_t z0 = stratum::javamath::floorDiv(z, kLatticePitch) * kLatticePitch;
    const std::array<double, 4> corners{field.at(x0, z0), field.at(x0 + kLatticePitch, z0),
                                        field.at(x0, z0 + kLatticePitch),
                                        field.at(x0 + kLatticePitch, z0 + kLatticePitch)};
    return ChunkFiller::preliminarySurfaceIn(corners, x - x0, z - z0);
}

/// How many of a source's two anchor coordinates are OFF the 16-block
/// lattice. On 0 the two readings are the same function of the field
/// (every sample is a lattice corner taken at full weight, the floor is
/// monotone and the abort threshold an integer), so only 1 and 2 can
/// discriminate.
[[nodiscard]] std::size_t anchorClass(const CellIndex centre) {
    const std::int32_t anchorX =
        stratum::javamath::floorDiv(centre.x, stratum::aquifer::kPslAnchorQuantum) *
        stratum::aquifer::kPslAnchorQuantum;
    const std::int32_t anchorZ =
        stratum::javamath::floorDiv(centre.z, stratum::aquifer::kPslAnchorQuantum) *
        stratum::aquifer::kPslAnchorQuantum;
    return static_cast<std::size_t>(stratum::javamath::floorMod(anchorX, kLatticePitch) != 0) +
           static_cast<std::size_t>(stratum::javamath::floorMod(anchorZ, kLatticePitch) != 0);
}

/// One rival reading's head-to-head with the shipped one, on the blocks
/// where the two predict different categories.
struct Duel {
    /// Blocks where the two readings predict different categories. A
    /// function of the readout fields and the centres alone: no aquifer
    /// world is read, so fluid flow cannot move it.
    long long differ = 0;
    /// Of those, where the server sides with the per-column reading...
    long long perColumnWins = 0;
    /// ...with the rival...
    long long rivalWins = 0;
    /// ...where its block is fluid that `explainedByFlow` says may have
    /// moved there, which either reading could then own...
    long long ambiguous = 0;
    /// (of which the per-column reading was the one predicting air, and of
    /// which the server's fluid is flowing rather than a source)
    long long ambiguousPerColumnAir = 0;
    long long ambiguousFlowing = 0;
    /// ...and where it placed a solid block, which neither predicts.
    long long solid = 0;

    void add(const Duel& other) {
        differ += other.differ;
        perColumnWins += other.perColumnWins;
        rivalWins += other.rivalWins;
        ambiguous += other.ambiguous;
        ambiguousPerColumnAir += other.ambiguousPerColumnAir;
        ambiguousFlowing += other.ambiguousFlowing;
        solid += other.solid;
    }
};

std::ostream& operator<<(std::ostream& out, const Duel& duel) {
    return out << "differ " << duel.differ << ": per column " << duel.perColumnWins << ", rival "
               << duel.rivalWins << ", ambiguous " << duel.ambiguous << " (per column air "
               << duel.ambiguousPerColumnAir << ", flowing " << duel.ambiguousFlowing << "), solid "
               << duel.solid;
}

struct HeadToHead {
    /// Model-only, like `Duel::differ`.
    long long positions = 0;
    long long sources = 0;
    /// Sources by `anchorClass`.
    std::array<long long, 3> sourcesByClass{};
    /// Sources on the 16-block lattice whose two scans differ: none can.
    long long tieMismatch = 0;
    long long sourcesReadDiffer = 0;
    long long sourcesLevelDiffer = 0;
    /// `lattice.differ` by `anchorClass`.
    std::array<long long, 3> differByClass{};
    /// Blocks where the per-column and lattice readings differ that sit above
    /// the chunk's `y_skip`, where the global picker decides and neither
    /// reading is consulted. Left out of every count above and below.
    long long differAboveYSkip = 0;
    /// Chunks whose `y_skip` rectangle leaves the readout, left out whole.
    long long chunksUnmeasured = 0;
    /// B: the 16-block lattice at the scan's own sample positions.
    Duel lattice;
    /// B': the same lattice at the unquantised centre plus the offsets.
    Duel latticeAtCentre;

    void add(const HeadToHead& other) {
        positions += other.positions;
        sources += other.sources;
        tieMismatch += other.tieMismatch;
        sourcesReadDiffer += other.sourcesReadDiffer;
        sourcesLevelDiffer += other.sourcesLevelDiffer;
        for (std::size_t c = 0; c < sourcesByClass.size(); ++c) {
            sourcesByClass.at(c) += other.sourcesByClass.at(c);
            differByClass.at(c) += other.differByClass.at(c);
        }
        differAboveYSkip += other.differAboveYSkip;
        chunksUnmeasured += other.chunksUnmeasured;
        lattice.add(other.lattice);
        latticeAtCentre.add(other.latticeAtCentre);
    }
};

std::ostream& operator<<(std::ostream& out, const HeadToHead& h) {
    return out << "positions " << h.positions << ", sources " << h.sources << " (by class "
               << h.sourcesByClass[0] << "/" << h.sourcesByClass[1] << "/" << h.sourcesByClass[2]
               << "), reads differ " << h.sourcesReadDiffer << ", levels differ "
               << h.sourcesLevelDiffer << ", tie mismatch " << h.tieMismatch << "; lattice "
               << h.lattice << " (by class " << h.differByClass[0] << "/" << h.differByClass[1]
               << "/" << h.differByClass[2] << "); lattice at centre " << h.latticeAtCentre
               << "; above y_skip " << h.differAboveYSkip << ", chunks unmeasured "
               << h.chunksUnmeasured;
}

/// One source under each reading, and whether its whole per-column window
/// lies in the footprint the aquifer world covers.
struct SourceLevels {
    bool reachable = false;
    bool counted = false;
    bool readsDiffer = false;
    std::int32_t perColumn = 0;
    std::int32_t lattice = 0;
    std::int32_t latticeAtCentre = 0;
    std::size_t anchorClass = 0;
};

[[nodiscard]] SourceLevels readSource(const Field& field, const CellIndex centre,
                                      const double floodedness) {
    bool reachable = true;
    const auto perColumn = [&](const std::int32_t sx, std::int32_t, const std::int32_t sz) {
        if (!Field::inside(sx, sz)) {
            reachable = false;
            return 0.0;
        }
        return field.at(sx, sz);
    };
    const PslRead readA = stratum::aquifer::readPreliminarySurface(perColumn, centre, kSeaLevel);
    // The population the per-column case scores, and no other: every one of
    // the thirteen samples in [0, 128). The lattice's far corners then sit
    // in [0, 128] on both readings below.
    if (!reachable) {
        return SourceLevels{};
    }
    const auto lattice = [&](const std::int32_t sx, std::int32_t, const std::int32_t sz) {
        return static_cast<double>(latticeSurface(field, sx, sz));
    };
    const PslRead readB = stratum::aquifer::readPreliminarySurface(lattice, centre, kSeaLevel);
    // The scan hands its sampler anchor + offset; adding back what the
    // 4-quantum took off moves every sample to centre + offset.
    const std::int32_t shiftX =
        stratum::javamath::floorMod(centre.x, stratum::aquifer::kPslAnchorQuantum);
    const std::int32_t shiftZ =
        stratum::javamath::floorMod(centre.z, stratum::aquifer::kPslAnchorQuantum);
    const auto latticeAtCentre = [&](const std::int32_t sx, std::int32_t, const std::int32_t sz) {
        return static_cast<double>(latticeSurface(field, sx + shiftX, sz + shiftZ));
    };
    const PslRead readBPrime =
        stratum::aquifer::readPreliminarySurface(latticeAtCentre, centre, kSeaLevel);

    const auto levelOf = [&](const PslRead& read) {
        return stratum::aquifer::cellFluidLevel(
            stratum::aquifer::CellFluid{.centreY = centre.y,
                                        .surface = read,
                                        .seaLevel = kSeaLevel,
                                        .floodedness = floodedness,
                                        .spread = 0.0});
    };
    return SourceLevels{.reachable = true,
                        .counted = false,
                        .readsDiffer = readA != readB,
                        .perColumn = levelOf(readA),
                        .lattice = levelOf(readB),
                        .latticeAtCentre = levelOf(readBPrime),
                        .anchorClass = anchorClass(centre)};
}

/// Q2.3/Q2.5's cutoff for chunk (@p cx, @p cz), its rectangle read through
/// @p surfaceAt; empty where the rectangle, or a lattice corner one of its
/// samples blends, leaves the readout.
template<typename SurfaceAt>
[[nodiscard]] std::optional<std::int32_t> ySkipOf(const Field& field, const std::int32_t cx,
                                                  const std::int32_t cz, SurfaceAt&& surfaceAt) {
    const stratum::aquifer::YSkipRectangle rect =
        stratum::aquifer::ySkipRectangle(cx * 16, cz * 16);
    if (!field.covers(rect.minX, rect.minZ) ||
        !field.covers(rect.maxX + kLatticePitch, rect.maxZ + kLatticePitch)) {
        return std::nullopt;
    }
    std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
    for (std::int32_t z = rect.minZ; z <= rect.maxZ; z += stratum::aquifer::kYSkipSampleStride) {
        for (std::int32_t x = rect.minX; x <= rect.maxX;
             x += stratum::aquifer::kYSkipSampleStride) {
            maxSurface = std::max(maxSurface, stratum::javamath::floorToInt(surfaceAt(x, z)));
        }
    }
    return stratum::aquifer::ySkip(maxSurface);
}

/// Scores one block where the per-column reading predicts @p perColumnFluid
/// and the rival the opposite.
void judge(stratum::test::GoldenRegion& golden, const std::int32_t x, const std::int32_t y,
           const std::int32_t z, const bool perColumnFluid, Duel& duel) {
    ++duel.differ;
    const auto* block = golden.blockAt(x, y, z);
    if (block == nullptr) {
        FAIL("no block at (" << x << ", " << y << ", " << z << ") in the aquifer world");
    }
    const stratum::test::Category server = stratum::test::categoryOf(block->name);
    if (server == stratum::test::Category::Solid) {
        ++duel.solid;
        return;
    }
    const bool serverFluid = stratum::test::isFluid(server);
    // The readings disagree, so exactly one predicted air; where the server
    // has fluid, that one lost — unless the fluid is a shape flow leaves, in
    // which case the air may have been right. Server AIR is never excused:
    // flow does not empty a block the aquifer filled.
    if (serverFluid &&
        stratum::test::explainedByFlow(golden, x, y, z, server, stratum::test::Category::Air)) {
        ++duel.ambiguous;
        duel.ambiguousPerColumnAir += static_cast<long long>(!perColumnFluid);
        duel.ambiguousFlowing += static_cast<long long>(stratum::test::fluidLevel(block) > 0);
    } else if (serverFluid == perColumnFluid) {
        ++duel.perColumnWins;
    } else {
        ++duel.rivalWins;
    }
}

/// Scores the rival readings against the per-column one on every block of
/// one aquifer world from the lava sea's top to `y_skip`, solid or not.
void headToHead(const std::filesystem::path& world, const Field& field, const std::int64_t seed,
                const double floodedness, HeadToHead& total) {
    const CentreSource centres{seed, stratum::density::RandomSource::Xoroshiro};
    stratum::test::GoldenRegion golden(world);
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, SourceLevels> seen;
    const auto perColumnAt = [&](std::int32_t x, std::int32_t z) { return field.at(x, z); };
    const auto latticeAt = [&](std::int32_t x, std::int32_t z) {
        return static_cast<double>(latticeSurface(field, x, z));
    };

    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            REQUIRE(golden.hasChunk(cx, cz));
            // Above `y_skip` the global picker decides, so a block there says
            // nothing about how the aquifer reads its surface. `y_skip` reads
            // the same entry, and through which reading is not this case's
            // question — so both are taken and the lower kept, which leaves
            // only blocks the local aquifer decides under either.
            const std::optional<std::int32_t> skipPerColumn = ySkipOf(field, cx, cz, perColumnAt);
            const std::optional<std::int32_t> skipLattice = ySkipOf(field, cx, cz, latticeAt);
            if (!skipPerColumn.has_value() || !skipLattice.has_value()) {
                ++total.chunksUnmeasured;
                continue;
            }
            const std::int32_t ceiling = std::min(*skipPerColumn, *skipLattice);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = stratum::aquifer::lambdaLevel(kSeaLevel); y < 200; ++y) {
                        const CellIndex centre =
                            stratum::aquifer::selectSources(centres, x, y, z).nearest().centre;
                        auto [found, fresh] = seen.try_emplace({centre.x, centre.y, centre.z});
                        if (fresh) {
                            found->second = readSource(field, centre, floodedness);
                        }
                        SourceLevels& levels = found->second;
                        if (!levels.reachable) {
                            continue;
                        }
                        const bool perColumnFluid = y < levels.perColumn;
                        if (y > ceiling) {
                            total.differAboveYSkip +=
                                static_cast<long long>(perColumnFluid != (y < levels.lattice));
                            continue;
                        }
                        if (!levels.counted) {
                            levels.counted = true;
                            ++total.sources;
                            ++total.sourcesByClass.at(levels.anchorClass);
                            total.tieMismatch += static_cast<long long>(levels.anchorClass == 0 &&
                                                                        levels.readsDiffer);
                            total.sourcesReadDiffer += static_cast<long long>(levels.readsDiffer);
                            total.sourcesLevelDiffer +=
                                static_cast<long long>(levels.perColumn != levels.lattice);
                        }
                        ++total.positions;
                        if (perColumnFluid != (y < levels.lattice)) {
                            ++total.differByClass.at(levels.anchorClass);
                            judge(golden, x, y, z, perColumnFluid, total.lattice);
                        }
                        if (perColumnFluid != (y < levels.latticeAtCentre)) {
                            judge(golden, x, y, z, perColumnFluid, total.latticeAtCentre);
                        }
                    }
                }
            }
        }
    }
}

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
        stratum::test::requireFrozen(world.parent_path().parent_path(),
                                     "tools/analysis/aquifer-psl-probe.sh");
        stratum::test::requireSeed(world.parent_path().parent_path(), 42);

        const Field field{readout};
        scoreWorld(
            world, [&](std::int32_t x, std::int32_t z) { return field.at(x, z); }, arm.floodedness,
            total);
    }

    REQUIRE(total.blocks > 5000000);
    INFO("blocks " << total.blocks << ", agree " << total.agree << ", flow " << total.flow
                   << ", server fluid ours air " << total.serverFluidOursAir
                   << ", server air ours fluid " << total.serverAirOursFluid << "; sources "
                   << total.sources << ", aborted " << total.aborted << ", prefix != whole "
                   << total.prefixDiffersFromWhole << ", distinct gate values "
                   << total.gateValues.size());

    // The corpus has to be able to say something before the score means
    // anything. All three arms must reach the gate, most sources must abort,
    // and the prefix minimum must differ from the whole-window minimum on a
    // large fraction — none of which a constant surface can produce.
    CHECK(total.gateValues.size() == 3);
    CHECK(total.aborted * 10 > total.sources * 9);
    CHECK(total.prefixDiffersFromWhole * 2 > total.sources);

    // Exact. The residual this case used to hold to a 99.0% bound — 34 878
    // blocks, all fluid this build calls air, piled at the lava sea's top
    // and just under `sea_level` — was fluid the server's ticks moved
    // before the save (support/fluid_flow.hpp), and the level rule was right
    // throughout. Frozen, the same corpus keeps 3 586 such blocks — a
    // remnant that differs between two frozen runs of one seed
    // (support/probe_corpus.hpp) — so that count is bounded, not pinned:
    // under 1 in 1000 blocks, where the unfrozen corpus stood at 4 in 1000.
    CHECK(total.serverFluidOursAir == 0);
    CHECK(total.serverAirOursFluid == 0);
    CHECK(total.agree + total.flow == total.blocks);
    CHECK(total.flow * 1000 < total.blocks);
}

TEST_CASE("the varying-surface harness reproduces the constant-surface law on its controls",
          "[conformance][aquifer]") {
    // The probe's three constant-surface controls, one at each arm value and
    // at the ladder's floodedness. They reproduce what about 1370 earlier
    // constant-surface dimensions showed, through the very path the varying
    // arms take — so a failure here is the harness measuring itself, not the
    // level rule. And they show the degeneracy the varying corpus exists to
    // escape: a constant surface yields one gate value and no prefix minimum
    // that differs from the whole window's.
    struct Control {
        const char* world;
        double surface;
    };

    for (const Control& control :
         {Control{"c96", kHigh}, Control{"cm20", kMid}, Control{"cm70", kLow}}) {
        INFO("control " << control.world);
        const std::filesystem::path world =
            fixtures() / "probes" / "pslvar" / control.world / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(world)) {
            SKIP("no varying-surface aquifer probe at "
                 << world << "; generate it with tools/analysis/aquifer-psl-probe.sh");
        }
        stratum::test::requireFrozen(world.parent_path().parent_path(),
                                     "tools/analysis/aquifer-psl-probe.sh");
        stratum::test::requireSeed(world.parent_path().parent_path(), 42);

        Score score;
        scoreWorld(world, [&](std::int32_t, std::int32_t) { return control.surface; }, 0.5, score);
        INFO("blocks " << score.blocks << ", agree " << score.agree << ", flow " << score.flow
                       << ", server fluid ours air " << score.serverFluidOursAir
                       << ", server air ours fluid " << score.serverAirOursFluid << "; sources "
                       << score.sources << ", aborted " << score.aborted << ", prefix != whole "
                       << score.prefixDiffersFromWhole);

        REQUIRE(score.blocks > 1000000);
        CHECK(score.serverFluidOursAir == 0);
        CHECK(score.serverAirOursFluid == 0);
        CHECK(score.agree + score.flow == score.blocks);
        // Measured 4, 1215 and 788 frozen; cm20 alone moved by 918 blocks
        // between two frozen runs, so this bound is loose on purpose — an
        // unfrozen corpus is refused above, not caught here.
        CHECK(score.flow * 100 < score.blocks);
        CHECK(score.gateValues.size() == 1);
        CHECK(score.prefixDiffersFromWhole == 0);
    }
}

TEST_CASE("the aquifer reads the surface per column and not through the 16-block lattice",
          "[conformance][aquifer]") {
    // The surface RULE reads `preliminary_surface_level` through a 16-block
    // lattice, blended and floored twice (vanilla_psl_lattice_test.cpp); the
    // aquifer reads the raw entry at each of its thirteen scan samples. Both
    // are shipped, and until this case only the first was measured. Here the
    // two readings of the aquifer's scan are scored head to head on the same
    // blocks: the per-column one (A, shipped) and the lattice one (B) at the
    // same anchor, window, scan order and abort — so this is "B with A's
    // window", not "B with a window refitted to it". B' is the lattice at the
    // unquantised centre plus the offsets, the one way a lattice reading
    // could stand in for the scan's 4-quantum.
    //
    // A source whose anchor is 16-aligned on both axes reads the same under
    // A and B for every possible field, so all of B's evidence comes from the
    // others; the case checks that tie class exists and never parts.
    HeadToHead total;
    std::map<std::string, HeadToHead> bySize;
    std::ostringstream perArm;

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
        stratum::test::requireFrozen(world.parent_path().parent_path(),
                                     "tools/analysis/aquifer-psl-probe.sh");
        stratum::test::requireSeed(world.parent_path().parent_path(), 42);

        // Two border rows past the footprint: the lattice's far corners at
        // 128, and `y_skip`'s rectangle's at 144.
        const Field field{readout, kChunks + 2};
        HeadToHead armTotal;
        headToHead(world, field, 42, arm.floodedness, armTotal);
        total.add(armTotal);
        bySize[arm.readout].add(armTotal);
        perArm << "\n  " << arm.world << ": " << armTotal;
    }

    const Duel& lattice = total.lattice;
    const Duel& latticeAtCentre = total.latticeAtCentre;
    INFO("total: " << total << perArm.str());

    // Power first: a handful of discriminating blocks would close nothing.
    // Measured 781 897, every feature size above 100 000 on its own.
    REQUIRE(lattice.differ >= 10000);
    for (const auto& [size, h] : bySize) {
        INFO("feature size " << size);
        CHECK(h.lattice.differ >= 10000);
    }

    // The tie class exists (378 of 2446 sources) and never parts.
    CHECK(total.sourcesByClass[0] > 0);
    CHECK(total.tieMismatch == 0);
    CHECK(total.differByClass[0] == 0);

    // Model-only, so pinned exactly: no aquifer world is read to get them,
    // and fluid flow cannot move them. The population is bounded by each
    // chunk's y_skip, so a change to y_skip moves it: 778 125 under engine v7,
    // 781 897 since v8 widened y_skip's rectangle (SPEC §6).
    CHECK(lattice.differ == 781897);
    CHECK(total.sourcesLevelDiffer == 350);

    // The verdict. The lattice wins no block, the per-column reading 661 223.
    CHECK(lattice.rivalWins == 0);
    CHECK(lattice.perColumnWins + lattice.rivalWins + lattice.ambiguous + lattice.solid ==
          lattice.differ);
    // Bounded, never pinned (SPEC §7). Measured 15 158 (2.3%): 1 580 flowing
    // and the rest still sources beside two more — `explainedByFlow`'s
    // infinite-source shape, which every interior block of a pool the
    // lattice would leave dry satisfies. On 1 856 of them it is the
    // per-column reading that predicted air: its own flow remnant, the one
    // the first case excuses.
    CHECK(lattice.ambiguous * 20 < lattice.perColumnWins);

    // B' loses the same way: 898 686 blocks differ, and the server sides
    // with it on none. Implied by the first case's exactness, and asserted
    // so the figure SPEC quotes is one a test holds.
    CHECK(latticeAtCentre.rivalWins == 0);
}

TEST_CASE("the per-column surface reading holds on three seeds with the barrier on",
          "[conformance][aquifer]") {
    // The same head-to-head on `aquifer-nsfloor-probe.sh`'s corpus: the same
    // three-valued field at feature sizes 8 and 16, with vanilla's barrier
    // noise on and floodedness 0.9 and 0, over three world seeds — so neither
    // the seed, the centres, the barrier held off nor the pslvar ladder's
    // floodedness carries the verdict. Seed 42's field is pslvar's own (same
    // noise, same seed); 31337 and 8675309 are new fields over new centres.
    //
    // Unlike pslvar, no case asserts this corpus's air and fluid exact under
    // the per-column reading, so the lattice winning no block here is not
    // implied by anything else in the suite.
    struct NsArm {
        const char* world;
        const char* readout;
        double floodedness;
    };

    constexpr std::array<NsArm, 4> kNsArms{{{"nsb_8", "nsr_8", 0.9},
                                            {"nsb_16", "nsr_16", 0.9},
                                            {"nsd_8", "nsr_8", 0.0},
                                            {"nsd_16", "nsr_16", 0.0}}};

    struct Probe {
        const char* name;
        std::int64_t seed;
        long long latticeDiffer;      ///< pinned: model-only
        long long sourcesLevelDiffer; ///< pinned: model-only
    };

    // Since engine v8's y_skip rectangle (530 231, 458 886 and 540 083
    // under v7).
    constexpr std::array<Probe, 3> kProbes{{{"nsfloor_s42", 42, 530567, 246},
                                            {"nsfloor_s31337", 31337, 459476, 206},
                                            {"nsfloor_s8675309", 8675309, 552725, 273}}};

    const std::filesystem::path root = fixtures() / "probes";
    if (std::ranges::none_of(kProbes, [&](const Probe& probe) {
            return std::filesystem::is_directory(root / probe.name);
        })) {
        SKIP("no nsfloor_s* aquifer probe under "
             << root << "; generate them with tools/analysis/aquifer-probes.sh");
    }

    for (const Probe& probe : kProbes) {
        const std::filesystem::path dir = root / probe.name;
        INFO("probe " << probe.name);
        // One seed's corpus present and another's missing is a broken
        // fixture set, not a smaller sample.
        REQUIRE(std::filesystem::is_directory(dir));
        stratum::test::requireFrozen(dir, "tools/analysis/aquifer-nsfloor-probe.sh");
        stratum::test::requireSeed(dir, probe.seed);

        HeadToHead total;
        std::ostringstream perArm;
        for (const NsArm& arm : kNsArms) {
            const std::filesystem::path readout = dir / arm.readout / "r.0.0.mca";
            const std::filesystem::path world = dir / arm.world / "r.0.0.mca";
            REQUIRE(std::filesystem::is_regular_file(readout));
            REQUIRE(std::filesystem::is_regular_file(world));
            const Field field{readout, kChunks + 2};
            HeadToHead armTotal;
            headToHead(world, field, probe.seed, arm.floodedness, armTotal);
            total.add(armTotal);
            perArm << "\n  " << arm.world << ": " << armTotal;
        }
        const Duel& lattice = total.lattice;
        INFO("total: " << total << perArm.str());

        REQUIRE(lattice.differ >= 10000);
        CHECK(total.sourcesByClass[0] > 0);
        CHECK(total.tieMismatch == 0);
        CHECK(total.differByClass[0] == 0);
        CHECK(lattice.differ == probe.latticeDiffer);
        CHECK(total.sourcesLevelDiffer == probe.sourcesLevelDiffer);

        // Measured per column 450 893, 382 386 and 476 317; lattice 0 on all
        // three.
        CHECK(lattice.rivalWins == 0);
        CHECK(lattice.perColumnWins + lattice.rivalWins + lattice.ambiguous + lattice.solid ==
              lattice.differ);
        // Measured 2 325, 25 388 and 443. Seed 31337's is the infinite-source
        // shape again: 10 651 on each of its two 16-block arms, only 42
        // flowing and all but 49 where the lattice predicted air — pools it
        // would leave dry.
        CHECK(lattice.ambiguous * 10 < lattice.perColumnWins);
        CHECK(total.latticeAtCentre.rivalWins == 0);
    }
}
