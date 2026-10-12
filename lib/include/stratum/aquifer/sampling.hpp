// Stratum — where the aquifer reads its router inputs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The level rule in `lattice.hpp` holds block for block on a constant surface
// (`aquifer-level-probe.sh`). Every probe behind it, though — the old
// campaign's 1370 dimensions and the 255 that hold it now — held
// `preliminary_surface_level` and `fluid_level_floodedness` at CONSTANTS. So
// the predicate was settled and the positions its inputs are read AT were not,
// and in a real world the surface varies per column and feeds the depth
// directly. This header is the answer to that question, for the two inputs
// where there is one.
//
// Measured by six agents across eleven world seeds, on instruments built
// independently of each other. The headline is that the inputs DO NOT share a
// sample position, and that is measured rather than inferred: on the same
// cells in the same worlds, the floodedness readout and the spread readout
// agree at 0.4895-0.5421 horizontally and 0.4986-0.5415 vertically, which is
// chance. Each was established on its own.
//
// There are FOUR of them now, and the fourth makes the point harder than the
// first three did. `lava` is read on a contracted lattice like the spread —
// and on a DIFFERENT horizontal pitch, 64 against the spread's 16. Two
// entries that look alike in shape and are not alike in numbers is exactly
// the pair an implementer collapses into one.
#pragma once

#include <stratum/aquifer/lattice.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace stratum::aquifer {

/// A position at which a noise-router entry is evaluated. Distinct from
/// `CellIndex` on purpose: two of the three reads below are NOT in block
/// coordinates, and conflating the two spaces is the mistake this type exists
/// to make hard.
struct SamplePos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    [[nodiscard]] constexpr bool operator==(const SamplePos&) const noexcept = default;
};

/// Where `fluid_level_floodedness` is read: the cell's own jittered centre, in
/// absolute block coordinates, verbatim. No quantisation, no offset, no
/// rounding, and no clamp — not even to the world floor, where a cell centred
/// below `min_y` still samples at its raw centre y.
///
/// One read per cell; the value is reused for every block that cell owns. A
/// per-block read is dead by three orders of magnitude rather than by a score:
/// it would have split 99.6% of cells under a one-block field and the server
/// split 4.8%.
///
/// The nearest rival is the centre quantised to two, at 0.618-0.627 against
/// this one's 1.0000, and it is wrong on all 136 cells where the two differ.
/// Also excluded on the same cells, all near or below the 0.498-0.574 chance
/// baseline: the cell's low corner, its midpoint, quantisation to 4, 8 and 16,
/// the centre plus or minus one on any axis (BELOW chance on y — a wrong
/// answer a majority-class baseline alone would have hidden), the cell index
/// as a coordinate, the jitter alone, every neighbouring cell, a fixed y at 0,
/// `min_y`, `sea_level` or the surface, the axes swapped, and the noise cell's
/// own corner. The worst of the full 16x16 grid of (x, z) candidates scores
/// 0.406; this one is first on every seed.
///
/// Spelled as a function of the centre rather than of the cell so that a
/// caller computes the centre once. It is the identity, and it is here to name
/// the finding and to make the asymmetry with `spreadSample` visible at the
/// call site.
[[nodiscard]] constexpr SamplePos floodednessSample(const CellIndex centre) noexcept {
    return SamplePos{.x = centre.x, .y = centre.y, .z = centre.z};
}

// `levelBand` — the 40-block band the spread is addressed by — lives in
// `lattice.hpp`, because the ladder is built from the same band.

/// Where `fluid_level_spread` is read — and it is NOT a position in block
/// space. The cell's lattice INDICES: the cell index in x and z, and the
/// 40-block band index in y.
///
/// Recovered without a candidate list, which is what makes it solid. Nine
/// dimensions each binary-encoding one bit of the sampled y gave per-band
/// purity of 1000/1000 on every bit and spelled the answer out directly; a
/// verifier repeated it with three independent 13-arm combs searching y over
/// [-2048, 2048] and read back the band index on all three of its own seeds.
/// The answer came off the server rather than out of a menu.
///
/// Excluded on the same cells: truncating the division (0.9417-0.9451), the
/// centre y itself — which is floodedness's own position — (0.4283-0.5416),
/// `floorDiv(centreY +- 20, 40)` (0.70-0.74), and every affine `40b + k` for k
/// in [-80, 120]. Horizontally: the centre (0.42-0.53), the corner
/// (0.43-0.53), the midpoint, any quantisation, and the axes swapped, all at
/// or below the 0.53-0.62 baseline. It is not an aggregate either: a minimum,
/// maximum or mean over the neighbouring bands scores 0.56-0.61, and a minimum
/// over the 3x3 index neighbourhood 0.43-0.47.
///
/// The x and z indices are `cell.x` and `cell.z` as given. `floorDiv(centre.x,
/// 16)` is provably the same number for every cell that can exist, since the
/// horizontal jitter never reaches 16, so the two spellings are a permanent
/// tie rather than an open question.
[[nodiscard]] SamplePos spreadSample(CellIndex cell, CellIndex centre) noexcept;

/// Where `lava` is read: the source's centre, contracted to indices — the
/// cell index on a SIXTY-FOUR block horizontal pitch, and the same 40-block
/// band in y that the spread and the ladder use.
///
/// The horizontal pitch is the measurement. On 3125 cells over four seeds the
/// type rule scores 0.99873 at pitch 64 and 0.9424, 0.9472 and 0.9418 at
/// pitches 16, 32 and 128 — all three at or below the 0.94176 null, so 64 is
/// not merely the best of the four, it is the only one that beats guessing.
///
/// `floorDiv`, never `/`: the aquifer runs on both sides of the origin and a
/// truncating division folds the cells either side of it into one. The probe
/// harness forceloads the origin quadrant, which is precisely how a
/// truncating reading of the surface anchor survived 1370 dimensions
/// unnoticed (see `kPslAnchorQuantum`).
[[nodiscard]] SamplePos lavaSample(CellIndex centre) noexcept;

/// The one axis of `preliminary_surface_level`'s read that IS settled: it is
/// evaluated at absolute y = 0, whatever the cell's centre y, the cell layer,
/// the world's `min_y` or the dimension's `sea_level`.
///
/// Three independent confirmations, and one of them approaches from outside:
/// a psl that differs from a constant only on y in [-1, 1] changes every
/// block, one that differs only on y in [300, 310] changes nothing, and 0 of
/// 6291456 blocks differ from the constant world otherwise. So it is a read
/// near y = 0 and specifically NOT a minimum taken over y. A `y_clamped_gradient`
/// ladder brackets it to [-0.5, +0.5), which is exactly 0 for an integer.
/// Excluded: `min_y`, `min_y + 64`, `sea_level`, and the cell's own centre y.
inline constexpr std::int32_t kPreliminarySurfaceSampleY = 0;

/// Q2.5's cutoff for the chunk whose minimum block corner is (@p baseX,
/// @p baseZ): `ySkip` of the highest FLOORED `preliminary_surface_level` over
/// `ySkipRectangle`, every `kYSkipSampleStride` blocks on both axes with both
/// endpoints included, each read at `kPreliminarySurfaceSampleY`. The one
/// spelling of the loop: `ChunkFiller` and the conformance cases call it.
///
/// @param psl anything callable as `double(std::int32_t x, std::int32_t y,
///            std::int32_t z)`, as for `readPreliminarySurface`.
template<typename Sampler>
[[nodiscard]] std::int32_t chunkYSkip(const Sampler& psl, const std::int32_t baseX,
                                      const std::int32_t baseZ) {
    const YSkipRectangle rectangle = ySkipRectangle(baseX, baseZ);
    std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
    for (std::int32_t z = rectangle.minZ; z <= rectangle.maxZ; z += kYSkipSampleStride) {
        for (std::int32_t x = rectangle.minX; x <= rectangle.maxX; x += kYSkipSampleStride) {
            // Java's `(int) Math.floor`: a datapack's psl may be NaN or out
            // of range, and a bare cast of either is undefined.
            maxSurface =
                std::max(maxSurface, javamath::floorToInt(psl(x, kPreliminarySurfaceSampleY, z)));
        }
    }
    return ySkip(maxSurface);
}

/// How `preliminary_surface_level`'s read is anchored: the cell's own jittered
/// centre, quantised down to a multiple of four. `floorDiv`, never `/` — the
/// two agree everywhere the probe harness looked, because it hardcodes a
/// forceload of the origin quadrant, and that is exactly how a truncating
/// reading survived 1370 dimensions unnoticed. Off the origin they part
/// decisively: 1294 exact readings give floorDiv 1.0000 against truncation
/// 0.16-0.55, and two later campaigns earned it again at short wavelength on
/// the negative fringe, 1.0000 against 0.771-0.955.
///
/// Quantising BEFORE offsetting versus after is a PERMANENT TIE, not a
/// measurement: every one of the thirteen offsets is a multiple of sixteen, so
/// `floorDiv(c, 4) * 4 + d` and `floorDiv(c + d, 4) * 4` are the same integer
/// identically. So is "an explicit anchor rule" versus "a quart cache wrapping
/// the entry". Separating either needs a consumer that reads the surface at a
/// position that is not a multiple of four, and none is known to exist.
inline constexpr std::int32_t kPslAnchorQuantum = 4;

/// The pitch of the window's lattice. Not the noise cell: `size_horizontal` 2
/// and 4 leave both this and the quantum unchanged (1.0000 on 356 readings,
/// while size-scaled variants score 0.09-0.55).
inline constexpr std::int32_t kPslStride = 16;

/// The value at which the scan gives up. Strict: -61.99 and -62.00 do not
/// fire, -62.01 does, each at 1.0000 at ordinary sea levels — but it is NOT
/// absolute, and that correction is measured rather than assumed.
///
/// `kLavaLevel - 8` fit every measurement through `sea_level` in
/// {40, 100, 128, 200}, and was flagged UNPROVEN because those never separate
/// it from a bare constant: `lambdaLevel(seaLevel)` equals `kLavaLevel` at
/// every one of them. A world with `sea_level` -70 does separate them, and
/// settles it decisively: eleven dimensions bisecting both candidates found
/// the boundary at exactly -78.00 / -78.01, matching
/// `lambdaLevel(-70) - 8 = -78` and refuting the bare -62 outright — every
/// dimension from -55 up through -78.00 is BYTE-IDENTICAL, and -78.01 alone
/// flips. So this is `lambdaLevel(seaLevel) - kOceanGateOffset`, which reduces
/// to the figure above (`kLavaLevel - 8 = -62`) at every ordinary sea level
/// this project had already verified, and moves with `sea_level` below -54
/// exactly as `lambdaLevel` itself does.
///
/// Invariant under `min_y` in {-48, -64, -80, -96, -128}, which kills the
/// "world floor plus two" reading outright — at `min_y` -128 that would put
/// the constant at -126, so a -70 arm would not abort, and it does.
[[nodiscard]] constexpr double abortThreshold(const std::int32_t seaLevel) noexcept {
    return static_cast<double>(javamath::wrappingSub(lambdaLevel(seaLevel), kOceanGateOffset));
}

/// The window, as offsets from the anchor, IN SCAN ORDER. The anchor itself is
/// not in the list: it is read first and separately.
///
/// This is not a square, and the asymmetry is measured rather than assumed —
/// it reaches 48 blocks west and 16 east, north and south. Three independent
/// non-parametric sieves, on seven seeds and both coordinate signs, marked an
/// offset impossible the moment one cell contradicted it and each arrived at
/// exactly these thirteen positions out of thousands of candidates. Every
/// dense or symmetric shape loses: the 4x3 without the spur scores 0.9261, the
/// 5x3 0.8366, the 3x3 0.7082, the 4x4 0.6732, the 5x5 0.4436, and a point
/// read 0.0700. Adding the spur's mirror at `(+32, 0)`, or its neighbours at
/// `(-48, +-16)`, violates outright. Nobody can explain why it is asymmetric.
///
/// THE ORDER IS LOAD-BEARING, because the scan aborts. `dz` outer ascending,
/// `dx` inner ascending, with the spur first in its row. Pinned twice: 0 of
/// 200 random permutations reach the winning score and all eleven adjacent
/// transpositions lose; separately, 19 rival orders score at most 0.9756
/// against 1.0000, with x-outer at 0.82-0.89 and z-descending at 0.77-0.87.
inline constexpr std::size_t kPslWindowSize = 12;

/// One offset from the anchor. Horizontal only — the window has no vertical
/// extent, and cells sharing a column at different layers each get their own
/// anchor and are predicted exactly.
struct PslOffset {
    std::int32_t dx = 0;
    std::int32_t dz = 0;

    [[nodiscard]] constexpr bool operator==(const PslOffset&) const noexcept = default;
};

inline constexpr std::array<PslOffset, kPslWindowSize> kPslWindow{{
    {.dx = -32, .dz = -16},
    {.dx = -16, .dz = -16},
    {.dx = 0, .dz = -16},
    {.dx = 16, .dz = -16},
    {.dx = -48, .dz = 0},
    {.dx = -32, .dz = 0},
    {.dx = -16, .dz = 0},
    {.dx = 16, .dz = 0},
    {.dx = -32, .dz = 16},
    {.dx = -16, .dz = 16},
    {.dx = 0, .dz = 16},
    {.dx = 16, .dz = 16},
}};

// Every offset sits on the measured pitch — the table is written out for
// readability, and this is what ties it to `kPslStride`.
static_assert(std::ranges::all_of(kPslWindow, [](const PslOffset offset) {
    return javamath::floorMod(offset.dx, kPslStride) == 0 &&
           javamath::floorMod(offset.dz, kPslStride) == 0;
}));

// `PslRead` — the four values one scan yields — lives in `lattice.hpp`,
// because it is what the level rule consumes.

/// Read `preliminary_surface_level` for one cell.
///
/// This is the shape that defeated three campaigns, and none of the pieces is
/// guessable. It is not a point sample: two worlds differing only in the low
/// arm of the surface function put the same cells on the same side of any
/// conceivable sample position, and yet 327 cells that sample the high arm in
/// both are entirely air in one and entirely water in the other. It is a
/// minimum — rank 0, confirmed on exact integer readings rather than on bits,
/// with the second-smallest at 0.17-0.48 and the median, mean and maximum at
/// 0.0000. And it aborts on a value, which is what made the earlier campaigns
/// see a "point read near the world floor": on a TWO-valued field the aborting
/// prefix-minimum is identically the point read, so a corpus that never varied
/// psl's values could not tell them apart and reported an exact 1.00000 for a
/// law that is wrong. A three-valued field separates them at once — of 425
/// cells where the minimum is the low arm and the point read is the high arm,
/// 258 return the MIDDLE arm, which neither model predicts.
///
/// TWO CONSUMERS, ONE SCAN. The gate stops at the aborting sample and the cap
/// does not. The argument is arithmetic rather than a fit: with `sea_level` 40,
/// floodedness 0.6 and a ladder at -20, the settled level rule yields 40 or -20
/// for EVERY psl, and both leave the cell wet — yet 858 of 858 such cells with
/// a non-centre sample below -62 are observed dry at the lava level, which
/// needs `min(ladder, psl) <= -54` in a branch only reachable when
/// `psl >= sea_level - 8`. No single psl value satisfies both. Constant-psl
/// controls at -70, -64, -63, -62, -58, -54, -40, 0, 40 and 100 all flood
/// those same cells, so it is the spike and not the value that empties them.
///
/// EACH SAMPLE IS THE RAW ENTRY AT ITS OWN COLUMN — not the value the surface
/// rule gets, which reads this same entry through a 16-block lattice, blended
/// and floored twice (`terrain::ChunkFiller::preliminarySurfaceIn`). Scored
/// head to head with the anchor, window, order and abort unchanged, on the
/// blocks where the two readings predict different categories: 802 159 on
/// the varying-surface probe, where the server sides with the per-column
/// reading on 679 973 and with the lattice on none, and 1 564 944 over three
/// seeds with the barrier on, 1 340 616 to none; every other block is solid
/// or fluid flow may have moved. The lattice taken at the unquantised centre
/// loses the same way. A source whose anchor is 16-aligned on both axes
/// reads the same under either, for every field, so all of the evidence is
/// from the others (`vanilla_aquifer_varying_surface_test.cpp`).
///
/// @param psl      anything callable as `double(std::int32_t x,
///                 std::int32_t y, std::int32_t z)` — the router entry, or a
///                 stub in a test.
/// @param centre   the cell's jittered centre, from `CentreSource::centreOf`.
/// @param seaLevel the dimension's `sea_level` — see `abortThreshold`. Every
///                 vector written before this parameter existed assumed an
///                 ordinary sea (>= -54), where it is a no-op; those vectors
///                 were updated to pass one explicitly rather than silently
///                 keep the old fixed threshold.
template<typename Sampler>
[[nodiscard]] PslRead readPreliminarySurface(const Sampler& psl, const CellIndex centre,
                                             const std::int32_t seaLevel) {
    const double abortBelow = abortThreshold(seaLevel);
    const std::int32_t anchorX =
        javamath::floorDiv(centre.x, kPslAnchorQuantum) * kPslAnchorQuantum;
    const std::int32_t anchorZ =
        javamath::floorDiv(centre.z, kPslAnchorQuantum) * kPslAnchorQuantum;

    // The anchor is read first, and its value seeds the prefix minimum
    // unconditionally — 200 of 200 cells whose own anchor sample is below the
    // threshold take THAT value rather than their neighbours' minimum, and
    // that half is reconfirmed on 6702 and 5268 further cells.
    //
    // It does NOT escape the abort, and this comment used to say it did. The
    // failure mode is worth naming: that experiment measured the VALUE of
    // `gate` on an aborting anchor, and the value was then read as evidence
    // about the state of the FLAG. The two are independent, and the cells that
    // separate them need an anchor below the threshold with every window
    // sample above it — a shape no corpus had built.
    const double seed = psl(anchorX, kPreliminarySurfaceSampleY, anchorZ);
    double prefix = seed;
    double whole = seed;
    // The anchor ARMS the abort. This line read `false` for a day, and the
    // difference shows only on a cell whose anchor is low while every window
    // sample is clean — 329 such cells across seventeen seeds and four
    // instruments, none of them backing the exemption.
    bool aborted = seed < abortBelow;

    for (const auto& offset : kPslWindow) {
        const double value =
            psl(anchorX + offset.dx, kPreliminarySurfaceSampleY, anchorZ + offset.dz);
        if (!aborted) {
            if (value < abortBelow) {
                // The aborting sample is NOT folded into the gate's minimum,
                // and the scan stops there — it does not skip and continue.
                // "Skip and continue" scores 0.8662 and a row-only break
                // 0.8864, against 1.0000 for stopping outright.
                aborted = true;
            } else {
                prefix = std::min(prefix, value);
            }
        }
        whole = std::min(whole, value);
    }

    // Floor toward negative infinity, on the double, once. Not round (0.4855),
    // not truncation toward zero (0.9209 — failing exactly on the negatives).
    // Java's `(int) Math.floor`, saturating and NaN -> 0: a datapack's psl can
    // be either, and a bare cast of them is undefined.
    // `cap` is the whole minimum whether or not the scan aborted: without an
    // abort every sample folded into the prefix too, and the two are one.
    return PslRead{.gate = javamath::floorToInt(prefix),
                   .cap = javamath::floorToInt(whole),
                   .anchor = javamath::floorToInt(seed),
                   .aborted = aborted};
}

/// One router read, memoized by position: the first call at a position asks
/// the wrapped sampler, and every later call at the same position returns
/// what it said then. For a read that is a pure function of its position —
/// and nothing else — this changes how often the sampler runs and nothing
/// about what any caller gets back.
///
/// Built for `preliminary_surface_level`, whose every read is a
/// `find_top_surface` column scan: a source's thirteen scan positions all sit
/// on the 4-aligned lattice its anchor quantum and the window's 16-block
/// pitch make, so neighbouring sources, and the sources of one cell column at
/// different layers, read many of the same positions — and `chunkYSkip`'s
/// own samples, 4-aligned over the same rectangle, cover every anchor a
/// chunk's sources can have. Measured on the shipped overworld (SPEC §11,
/// "What the aquifer costs").
///
/// Per-task scratch, like `StatusCache`: the caller owns one for as long as
/// the wrapped read stays the same function — in `ChunkFiller::fill`, one
/// chunk, since its reads go through that chunk's flat_cache window — and
/// never shares it between threads. Not callable through a const reference:
/// a caller hands `readPreliminarySurface` a lambda that calls it.
template<typename Sampler>
class MemoizedRead {
public:
    explicit MemoizedRead(Sampler sampler) : sampler_(std::move(sampler)) {}

    [[nodiscard]] double operator()(const std::int32_t x, const std::int32_t y,
                                    const std::int32_t z) {
        const Key key{.x = x, .y = y, .z = z};
        if (slots_.empty()) {
            slots_.resize(kInitialSlots);
        }
        std::size_t slot = probe(key);
        if (slots_[slot].used) {
            return slots_[slot].value;
        }
        // Asked first and stored after: a read that throws stores nothing,
        // and the next call at the same position asks again.
        const double value = sampler_(x, y, z);
        if ((size_ + 1) * 2 > slots_.size()) {
            grow();
            slot = probe(key);
        }
        slots_[slot] = Slot{.key = key, .value = value, .used = true};
        ++size_;
        return value;
    }

    /// How many distinct positions the wrapped sampler has been asked for.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Key {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;

        [[nodiscard]] bool operator==(const Key&) const noexcept = default;
    };

    struct Slot {
        Key key{};
        double value = 0.0;
        bool used = false;
    };

    /// Open addressing in ONE vector, grown by doubling at half full: a
    /// node-per-entry map scatters small allocations between the
    /// interpreter's own per-read ones, and measured, that cost the terrain
    /// reads around it more than the map saved.
    static constexpr std::size_t kInitialSlots = 1024;

    /// Where @p key sits, or the empty slot it would take. Any mix will do —
    /// the hash decides where an entry is stored, never what it holds.
    /// Unsigned throughout, so it wraps by definition.
    [[nodiscard]] std::size_t probe(const Key& key) const noexcept {
        constexpr std::uint64_t kMix = 0x9e3779b97f4a7c15ULL;
        std::uint64_t h = static_cast<std::uint32_t>(key.x);
        h = (h * kMix) ^ static_cast<std::uint32_t>(key.y);
        h = (h * kMix) ^ static_cast<std::uint32_t>(key.z);
        h *= kMix;
        const std::size_t mask = slots_.size() - 1;
        auto slot = static_cast<std::size_t>(h >> 32U) & mask;
        while (slots_[slot].used && !(slots_[slot].key == key)) {
            slot = (slot + 1) & mask;
        }
        return slot;
    }

    void grow() {
        std::vector<Slot> old(slots_.size() * 2);
        old.swap(slots_);
        for (const Slot& entry : old) {
            if (entry.used) {
                slots_[probe(entry.key)] = entry;
            }
        }
    }

    Sampler sampler_;
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

/// WHAT THE FOUR VALUES ANSWER. The clean-room spec's Q5.3 is a first match
/// over the thirteen samples in scan order — Q5.3(a) on the anchor, then the
/// first sample that is submerged and that the centre sits above less four —
/// and `cellLevel` answers it from this reduction without the samples:
/// `anchor` is Q5.3(a)'s comparand; `gate` fires exactly when a clean sample
/// before the first abort does (the minimum fires when any does), and sits
/// below the threshold, firing nothing clean, when the anchor itself aborts;
/// past the abort only an aborting sample can fire, and the lowest does if
/// any does, so `cap - centreY < 4` says one fired; and `cap` is Q5.4's
/// minimum for the level rule. `aquifer-ties-probe.sh` measured that reading
/// against the one engine v12 made of the same values, on every kind of
/// source where they part, and the server sides with the spec on every block
/// (SPEC §11, "The surface scan, sample by sample").
///
/// THREE TIES THIS NOTE CARRIED, now measured. The near-surface exemption's
/// comparand (`centreY > cap + 20`) was called a permanent tie between the
/// whole window's minimum, the aborting sample and any sentinel at or below
/// -54, on the argument that `cap` reached a level only through a clamped
/// ladder; once the clamp went it reached the barrier as a number, and the
/// comparand is neither — it is Q5.3(a)'s anchor, which is the aborting
/// sample exactly when the anchor aborts (the ties probe's `ta` and `tab`
/// worlds: 106 393 blocks for the anchor over the minimum; `tb` and `tab`:
/// 69 935 against the aborting sample where it is not the anchor). Q5.3(a)'s
/// comparand for a scan that did not abort, `cap` or the anchor, which a
/// constant surface makes one number: the anchor (`tl35`, `tl43`, through
/// the sea's lid, 1 780 blocks). And whether the first submerged sample in
/// scan order decides, which the abort flag had stood in for: it does (`tb`,
/// `tbr`, `tab`, 64 913 blocks; a land prefix before an abort, `tc`, `tcr`,
/// `tp35`, `tp43`, 26 389 blocks and 6 472 marks).
///
/// Whether the anchor arms the abort was ALSO listed here as undecidable, and
/// it is not: 329 cells over seventeen seeds decide it, and the armed reading
/// is right on every one. Where the flag is TESTED was carried here as a
/// permanent tie — "the anchor arms it" against "the near-surface return
/// additionally requires the anchor to clear the threshold" — and it is not
/// one either: on `aquifer-lowfloor-probe.sh`'s worlds the server sides with
/// the first on every sampled block where they part, at sea -70, -60 and 63
/// alike (vanilla_aquifer_lowfloor_test.cpp). Under the spec's reading the
/// question does not arise: an aborting anchor is simply the first sample.
///
/// What no world can part, and why: a source four or more below every
/// sample of an aborting scan fires none, and takes the level rule where v12
/// gave A_lava; such a centre sits at least thirteen below lambda, so only Π
/// just above lambda reaches it, and the ties probe's `td` world, built for
/// it, shows 14 blocks over two seeds, all on row lambda (the spec's on all
/// 14). That is a thin
/// measurement, not a tie. The permanent ties that remain are arithmetic:
/// `kPslAnchorQuantum`'s before-or-after offset and `spreadSample`'s two
/// spellings.
///
/// With the anchor armed, `aborted` and `cap <= -63` are the SAME predicate
/// over every field that can exist at an ordinary sea — `cap` is the whole
/// window's minimum always, and -62 is an integer, so `floor(m) <= -63`
/// exactly when `m < -62`. The tests assert that identity.

} // namespace stratum::aquifer
