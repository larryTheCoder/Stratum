// Stratum — evaluating a resolved density function graph, one point at a time.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §4.1 permits exactly this as a Milestone-M2 stepping stone: "the
// resolved graph is compiled into a flat execution program ... interpretation
// of the graph per block is acceptable only as a Milestone-M2 stepping
// stone." The compiled program was due in M3 and was not built; it is
// deferred to M5's performance pass (SPEC §10, M3). Its first part is here:
// a root's per-block arithmetic over its `interpolated` values, the part
// evaluated at every block, runs as a straight list of ops (BlockKernel).
// Everything else — every cell corner, every read off the block — is still
// this walk.
//
// WHAT IT WILL NOT DO. A node type it cannot evaluate correctly at a point
// is refused by name, with the reason (SPEC §8). That covers two groups:
//
//   * the cell-structured ones — `interpolated`, `cache_all_in_cell`,
//     `slide`, `find_top_surface` — whose value is not defined by the point
//     alone. The first two are defined by the *cell*, so an interpreter
//     given a CellGeometry evaluates them and one without still refuses
//     them; the other two need more than a cell and are still refused;
//   * the ones with neither documentation nor an oracle here —
//     `old_blended_noise`, `end_islands`, `weird_scaled_sampler`,
//     `blend_density`. Guessing a formula for these would produce a world
//     that generates and is silently wrong, which is the failure mode this
//     project treats as most severe.
//
// A node whose `noise` field carried its parameters inline is refused too,
// but for a different reason, and the difference is the whole point of
// UnbuildableError below. The input is legal — mcdoc declares the field as an
// identifier OR a noise — and the graph resolves it. It is *vanilla* that
// cannot build it: the server loads such a pack without complaint and then
// dies constructing the dimension's random state, because a noise is seeded
// from the MD5 of its identifier and one written inline has none. Measured,
// not reasoned: tools/analysis/inline-noise-probe.sh, SPEC §11.
//
// Everything else is evaluated. The 2D chain vanilla's overworld actually
// uses — shift_a/shift_b, flat_cache, cache_2d, shifted_noise, noise, spline
// and the arithmetic around them — is checked bit-exactly against cubiomes
// in tests/conformance/vanilla_climate_test.cpp.
//
// CACHE NODES. CLAUDE.md calls these parity-critical, and they are, so it is
// worth being exact about what this layer does with them. `flat_cache`
// relocates its sample to the corner of the 4x4 column it sits in, at y=0 —
// inside the window a chunk's grid covers, when a caller passes one — and
// that changes the value at a point, so it is implemented. `cache_2d`,
// `cache_once` and `cache_all_in_cell` are memoisation that does not move the
// sample, so at a single point they are transparent — and `cache_2d` is
// transparent only because what vanilla wraps in it never varies with y,
// which this layer checks rather than assumes. `interpolated` blends its
// argument over the cell the point sits in, which needs a CellGeometry (it is
// refused without one) and applies only to the block being generated: a
// point that block reads elsewhere reads the argument itself (ReadContext).
// All of that is measured at the aquifer's reads, the only place a point is
// read off its own block (SPEC §11, "Cache markers in a datapack's aquifer
// entries").

#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/noise/blended.hpp>
#include <stratum/noise/end_islands.hpp>
#include <stratum/noise/perlin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace stratum::density {

/// The rarity `weird_scaled_sampler` gives an input, for one of the two
/// mappers vanilla defines.
///
/// A ladder of thresholds, compared with a strict `<`, so a value sitting
/// exactly on a threshold takes the rarity ABOVE it. That is measured, not
/// assumed: every threshold was probed at the value itself and at plus and
/// minus 1e-7 either side, and the step falls exactly on it (SPEC §11).
///
/// Which ladder belongs to which name is the part worth being careful about.
/// minecraft.wiki documents both ladders but its two pages assign them to
/// `type_1` and `type_2` in OPPOSITE order, so the names could not be taken
/// from documentation at all. The assignment here is the one the vanilla
/// server itself produces.
///
/// Throws EvalError for a mapper this build does not know, rather than
/// picking one — a silently wrong rarity would reshape every cave.
[[nodiscard]] double rarityValueMapper(std::string_view mapper, double input);

/// Raised when a graph cannot be evaluated: a node type this build does not
/// implement, a missing noise, a cache whose contents break its contract.
///
/// Its meaning is "this build cannot do that *yet*" — the pack is fine and
/// the engine intends to run it (SPEC §10). For input no build ever will,
/// see UnbuildableError below.
class EvalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Raised for input that is legal to write, legal to load, and that vanilla
/// itself will not build a world from — so refusing it is parity rather than
/// an admission (SPEC §8, §11).
///
/// Today that is exactly one thing: a `noise` field carrying its parameters
/// inline. It derives from EvalError because every caller that wants "can
/// this be sampled?" wants one answer to that question; what the separate
/// type buys is that `stratum validate` can tell a broken pack from an
/// incomplete engine, and report the first as an error rather than a note
/// about this build.
class UnbuildableError : public EvalError {
public:
    using EvalError::EvalError;
};

/// The lattice vanilla samples terrain density on: density is evaluated at
/// cell corners and interpolated within, which is what `interpolated` means
/// and why its value is not a function of the point alone (SPEC §4.1).
///
/// Both dimensions come from a noise settings entry — `size_horizontal` and
/// `size_vertical` times four — and are passed in rather than read from one,
/// so that the density layer does not have to know what a dimension is.
struct CellGeometry {
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] bool operator==(const CellGeometry& other) const noexcept = default;
};

/// Where a density function is being evaluated, in block coordinates.
struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    [[nodiscard]] bool operator==(const Point& other) const noexcept = default;
};

/// The chunk coordinates a dimension generates, on either axis:
/// |chunk| <= kMaxChunkCoordinate, 2^27 - 8. A chunk's blocks are formed as
/// 16 * chunk + local in int32, and its reads reach past its edge — a cell
/// corner at +16, the `flat_cache` window to +19, an aquifer source centre to
/// about +47 — so near 2^27 chunks that arithmetic leaves int32 and is
/// undefined (chunk 134217727's window, 2147483632 + 20, was). This keeps a
/// margin of more than a hundred blocks at both ends. terrain::ChunkFiller::
/// fill and biome::Source::fillChunk refuse a chunk outside it by name
/// (SPEC §8). Vanilla's world border sits at ±29 999 984 blocks
/// (minecraft.wiki), under 1/70 of this, so no vanilla world has such a
/// chunk.
inline constexpr std::int32_t kMaxChunkCoordinate = (1 << 27) - 8;

/// Whether (@p chunkX, @p chunkZ) is inside kMaxChunkCoordinate.
[[nodiscard]] constexpr bool chunkInDomain(const std::int32_t chunkX,
                                           const std::int32_t chunkZ) noexcept {
    return chunkX >= -kMaxChunkCoordinate && chunkX <= kMaxChunkCoordinate &&
           chunkZ >= -kMaxChunkCoordinate && chunkZ <= kMaxChunkCoordinate;
}

/// The columns one chunk's `flat_cache` grid holds, in block coordinates,
/// both ends inclusive.
///
/// A `flat_cache` read INSIDE the window is relocated to its 4x4 column
/// corner at y = 0 — the grid point that holds it. A read OUTSIDE has no grid
/// point to be relocated to and reads its argument where it was asked, its
/// own y included. Every in-chunk density read sits inside its own chunk's
/// window, so the distinction is invisible to terrain; it surfaces only for a
/// read the chunk makes off its own footprint, which in the vanilla presets
/// means one thing — the aquifer's Q5.9 erosion and depth at a source centre
/// in a neighbouring chunk — and in a datapack, any aquifer entry wrapped in
/// it (SPEC §11). Its extent is measured, not assumed: see `ChunkFiller`.
///
/// `none()` covers nothing: every read is where it was asked. Evaluating
/// without a window at all relocates every read, which is what a caller
/// with no chunk in hand (a probe, a test, `stratum render`) has always got.
struct FlatCacheWindow {
    std::int32_t minX = 0;
    std::int32_t maxX = -1;
    std::int32_t minZ = 0;
    std::int32_t maxZ = -1;

    [[nodiscard]] static constexpr FlatCacheWindow none() noexcept { return FlatCacheWindow{}; }

    [[nodiscard]] constexpr bool covers(const std::int32_t x, const std::int32_t z) const noexcept {
        return x >= minX && x <= maxX && z >= minZ && z <= maxZ;
    }
};

/// Values for the node types SPEC §11 leaves unsettled, supplied by an
/// experiment that is trying to settle them.
///
/// Absent by default, and that default is the point: without it those types
/// stay refused, so no ordinary caller can generate a world from a reading
/// nothing has verified. An experiment comparing generated terrain against
/// the goldens is the one thing that *can* settle them, and it needs a way
/// to put a candidate in front of the pipeline.
struct UnsettledSubstitutions {
    /// What `old_blended_noise` samples. The candidate under test — see
    /// stratum::noise::BlendedNoise for the two questions it answers.
    std::function<double(const noise::BlendedNoise::Parameters&, Point)> blendedNoise;

    /// What `weird_scaled_sampler` returns, ignoring its noise and input.
    ///
    /// The only use for this is isolation. Vanilla's caves reach terrain
    /// through nested `min`s, so a large positive constant here loses every
    /// one of those and leaves the terrain shape alone — without pretending
    /// to know the rarity mapping, which stays unsettled. An experiment that
    /// uses it must check its own premise: caves only ever remove material,
    /// so a prediction made this way has to come out at or above vanilla's
    /// surface in every column, and any column below it means the isolation
    /// is wrong rather than the candidate.
    std::optional<double> weirdScaledSampler;

    [[nodiscard]] bool empty() const noexcept {
        return blendedNoise == nullptr && !weirdScaledSampler.has_value();
    }
};

/// Whose read an evaluation is. Only `interpolated` tells the two apart.
///
/// MEASURED, not chosen (SPEC §11, "Cache markers in a datapack's aquifer
/// entries", `tools/analysis/aquifer-markers-probe.sh`): with an aquifer
/// entry wrapped in `interpolated`, the barrier — read at the block being
/// generated — is the blend over that block's cell, and every other read the
/// aquifer makes for that block — a source's centre, its contracted spread
/// and lava indices, its surface anchors — is the argument at the point
/// itself. Off the block the blend is wrong on 61 295 to 785 454 blocks an
/// arm and seed, and the argument exact on every block; at the block the
/// argument is wrong on 14 878 blocks over four arm-seeds, and the blend
/// exact.
enum class ReadContext : std::uint8_t {
    /// The block being generated, or a cell corner it is blended from.
    Block,
    /// A point the generating block reads somewhere else.
    Detached,
};

class Interpreter {
    /// The storage behind every Scope one top-level call opens: the memo a
    /// point's evaluation keeps, as a stack of frames, one per level of
    /// nesting — an evaluate(), an interpolated corner inside it, a
    /// flat_cache relocation inside that, a find_top_surface step.
    ///
    /// WHY IT EXISTS. A Scope used to own two vectors sized to the whole
    /// graph — every dimension's nodes, 730 for vanilla's pack — so every
    /// point, corner, relocation and scan step paid two allocations and
    /// about 6.5 KB of zeroing for a scope that executes thirteen nodes on
    /// average: 202 000 scopes and 1.3 GB of memset per overworld chunk.
    /// Here a frame is allocated once per level and reused by every scope
    /// that opens at that level.
    ///
    /// WHAT REPLACES THE ZEROING is an epoch per frame. Each slot carries
    /// the epoch of the scope that wrote it, and a node is computed for a
    /// scope exactly when its slot carries that scope's epoch. Opening a
    /// scope takes the frame's next epoch, which no slot carries yet —
    /// every stamp is at most the frame's current epoch — so every node
    /// reads as not computed, which is what a freshly zeroed vector said.
    /// When the counter wraps, the frame's stamps are cleared and counting
    /// restarts at 1, so a stamp from before the wrap can never be taken
    /// for the work of a scope after it. Nothing else changes: the same
    /// nodes are computed in the same order, and the memo holds the same
    /// values for the same scope.
    ///
    /// It belongs to one top-level call at a time — through the caller's
    /// CornerCache, or local to evaluate(root, at) — and never to the
    /// interpreter, which is shared between threads and stays immutable
    /// (SPEC §4.1).
    class ScopeArena {
    public:
        /// One node's memo entry in a frame.
        struct Slot {
            double value = 0.0;
            /// The epoch of the scope that wrote the value; 0 for never.
            std::uint32_t stamp = 0;
        };

        /// What one scope holds of the frame it was given: where its
        /// slots are, and the epoch that marks a slot as its own.
        struct Lease {
            Slot* slots = nullptr;
            std::uint32_t epoch = 0;
        };

        /// The frame one level deeper than every scope still open, sized
        /// for @p nodeCount nodes and at a fresh epoch, in which nothing
        /// reads as computed. If it throws (an allocation), nothing was
        /// leased and leave() must not be called.
        [[nodiscard]] Lease enter(std::size_t nodeCount);

        /// Gives back the frame the innermost open scope was leased.
        void leave() noexcept { --depth_; }

        /// Moves every frame's epoch counter forward to at least @p epoch,
        /// including the frames not allocated yet; never back. For tests:
        /// see CornerCache::advanceScopeEpochs.
        void advanceEpochs(std::uint32_t epoch) noexcept;

        /// How many times a frame's epoch counter has wrapped.
        [[nodiscard]] std::uint64_t wraps() const noexcept { return wraps_; }

    private:
        struct Frame {
            std::vector<Slot> slots;
            /// The epoch of the scope that holds or last held this frame.
            /// Every stamp in `slots` is at most this.
            std::uint32_t epoch = 0;
        };

        /// Open scopes hold raw pointers into these frames' slots, so a
        /// frame must keep its slot buffer when the vector of frames grows.
        /// A moved std::vector keeps its buffer, and the vector of frames
        /// moves rather than copies its elements when their move cannot
        /// throw; enter() asserts that it cannot.
        std::vector<Frame> frames_;
        /// How many scopes are open, which is the index of the next frame.
        std::size_t depth_ = 0;
        /// The epoch a frame allocated from now on starts from.
        std::uint32_t floor_ = 0;
        std::uint64_t wraps_ = 0;
    };

    /// Values a CornerCache's evaluations have already computed and may need
    /// again at another point, keyed by everything each value depends on
    /// (see CornerCache).
    ///
    /// An open-addressed table with a full key in every slot, so a miss is
    /// only ever a recomputation and never a wrong value: a slot is used only
    /// when all five words of its key match. Bounded: it grows to
    /// kMaxCapacity slots and is then forgotten whole and refilled, which
    /// costs recomputation and changes nothing. Forgetting is an epoch, as in
    /// ScopeArena: a slot holds a value exactly when its stamp is the table's
    /// epoch.
    ///
    /// Nothing is held between a lookup and an insert — an evaluation in
    /// between may grow or forget the table — so neither returns a pointer
    /// into it.
    class MemoTable {
    public:
        /// The node, three coordinates (as their int32 bits) and a tag that
        /// says what kind of value it is and what it was read through.
        using Key = std::array<std::uint32_t, 5>;

        /// The table's largest size, in slots of 32 bytes: 2 MiB, of which
        /// it fills at most half. A vanilla chunk's fill keeps 15 000 to
        /// 20 000 values (measured over the bench's overworld, amplified and
        /// End chunks), inside that.
        static constexpr std::size_t kMaxCapacity = std::size_t{1} << 16;

        /// Whether @p key is held, and if so its value in @p value.
        [[nodiscard]] bool find(const Key& key, double& value) const noexcept;

        /// Holds @p value under @p key, replacing what the key held.
        void insert(const Key& key, double value);

        /// Forgets every value.
        void clear() noexcept;

        /// How many values it holds.
        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        /// Moves the epoch forward to at least @p epoch, never back, which
        /// forgets every value as clear() does. For tests: see
        /// CornerCache::advanceMemoEpoch.
        void advanceEpoch(std::uint32_t epoch) noexcept;

        /// How many times the epoch has wrapped.
        [[nodiscard]] std::uint64_t wraps() const noexcept { return wraps_; }

    private:
        struct Slot {
            Key key{};
            /// The table's epoch when this slot was written; 0 for never.
            std::uint32_t stamp = 0;
            double value = 0.0;
        };

        [[nodiscard]] static std::size_t hash(const Key& key) noexcept;

        /// Where @p key is, or the empty slot where it would go.
        [[nodiscard]] std::size_t probe(const Key& key) const noexcept;

        /// Twice the slots, every value moved across.
        void grow();

        std::vector<Slot> slots_;
        std::uint32_t epoch_ = 1;
        std::size_t size_ = 0;
        std::uint64_t wraps_ = 0;
    };

public:
    /// Binds a graph to the noises it names. Throws EvalError if any `noise`
    /// field has nothing behind it, so that a missing noise is found once at
    /// construction rather than on whichever chunk first reached that node.
    Interpreter(const Graph& graph, const NoiseRegistry& noises);

    /// Puts candidate readings of the unsettled types in front of the
    /// pipeline. For experiments only; see UnsettledSubstitutions.
    void substitute(UnsettledSubstitutions substitutions);

    /// The same, over a cell lattice. `interpolated` and `cache_all_in_cell`
    /// become evaluable; without one they stay refused, because there is no
    /// honest value for them.
    ///
    /// Throws EvalError for a geometry that is not positive in both
    /// directions: a zero-width cell would divide by zero on the first
    /// interpolation, which is a worse way to learn about it.
    Interpreter(const Graph& graph, const NoiseRegistry& noises, CellGeometry cells);

    /// Refuses, by name and with a reason, anything in @p root's subtree
    /// this build cannot evaluate at a point. Callers that intend to sample
    /// a function many times should call this once first: evaluate() raises
    /// the same errors, but on the sample rather than at load.
    void requireEvaluable(NodeIndex root) const;

    /// Per-task scratch for filling a chunk, holding the cell corner values
    /// `interpolated` nodes need.
    ///
    /// WHY THIS EXISTS. `interpolated` is defined over a cell: its value at a
    /// point is a blend of its argument at the eight corners of the cell the
    /// point sits in. Evaluating it at a point therefore costs eight
    /// evaluations of the argument, and a filler asking for every block in a
    /// 4x8x4 cell pays that 128 times over for eight values that never
    /// change. Vanilla computes them once per cell; so does this, when the
    /// caller supplies somewhere to keep them.
    ///
    /// It is the CALLER's, not the interpreter's, because a compiled pipeline
    /// is immutable and shared between threads (SPEC §4.1 and §5) — a cache
    /// hanging off the interpreter would be shared mutable state in the hot
    /// path. One of these per generating task, never two tasks to one.
    ///
    /// Using one is not supposed to change any value it returns. That is
    /// asserted rather than assumed: see the unit tests, which compare cached
    /// and uncached evaluation bit-for-bit over a whole cell.
    ///
    /// It also carries the frames every evaluation through it keeps its
    /// per-point memo in (ScopeArena), so that a chunk allocates those once
    /// rather than twice for every point, corner and relocation it reads.
    /// They hold nothing from one evaluation to the next: every scope
    /// starts as empty as a fresh one, which the unit tests assert across
    /// a wrap of the epoch counter.
    ///
    /// AND IT REMEMBERS WHAT IS THE SAME FROM ONE POINT TO THE NEXT, in a
    /// MemoTable (Sharing::Lattice), which is where most of a chunk's
    /// density work went:
    ///
    ///   * the corner lattice. A corner belongs to up to eight cells, and
    ///     each `interpolated` argument used to keep only its last cell's
    ///     eight, so a chunk evaluated 28 000 corners for 5 700 distinct
    ///     ones. Each corner is now kept by its argument, its position and
    ///     what it was read through, and filled lazily: a corner is
    ///     evaluated the first time a cell that has it is read, in that
    ///     cell's corner order, and never if no such cell is read. So the
    ///     corners evaluated are the same set as before, in the same order
    ///     of first evaluation, and the first refusal is the same one;
    ///   * `flat_cache`'s relocated reads: its argument at the column corner
    ///     (floorDiv(x, 4) * 4, 0, floorDiv(z, 4) * 4), which every block of
    ///     that 4x4 column reads, and which was evaluated afresh on every
    ///     read — 30 000 times an overworld chunk;
    ///   * the column memo: a node with one value at every y of its column
    ///     (Op::columnMemo), such as the five climate entries a biome is
    ///     chosen by, kept per (x, z) rather than recomputed at every y.
    ///
    /// Each value is kept under everything it depends on — the node, its
    /// point or column, the window it was read through (when it can depend
    /// on one) and the read context — and only once its evaluation has
    /// returned. What the memo skips is only a repeat of an evaluation that
    /// already returned, of the same node where it returns the same double;
    /// a refusal is thrown at the same point and in the same words. The unit
    /// tests compare it with uncached evaluation bit for bit over whole
    /// chunks, and a conformance case holds every node of vanilla's routers
    /// the column memo keeps to one bit pattern down its column. None of it
    /// is used while the interpreter carries UnsettledSubstitutions, whose
    /// functions nothing here can prove pure.
    class CornerCache {
    public:
        /// How the cache serves `interpolated`'s corners, and whether it
        /// keeps anything else.
        enum class Sharing : std::uint8_t {
            /// The memo: every corner kept by argument, position and — when
            /// the argument reaches a `flat_cache` — the window it was read
            /// through, the last cell's corners each argument read kept the
            /// same way, and `flat_cache`'s relocated reads and the column
            /// memo kept by node, column, window and context. Each value is
            /// a pure function of its key, so one cache can serve reads
            /// through any windows.
            Lattice,
            /// What every CornerCache did before the memo: each argument
            /// keeps its last cell's eight corners, whichever window they
            /// were read through, and nothing else. A cache that serves
            /// Block reads of one corner through two windows that tell its
            /// value apart answers by which read came first; that is this
            /// mode's semantics, kept bit for bit for the one caller that can
            /// meet it (terrain::ChunkFiller, when a pack's `barrier` reaches
            /// an `interpolated` over a `flat_cache`).
            LastCell,
        };

        explicit CornerCache(std::size_t nodeCount) : entries_(nodeCount) {}

        CornerCache(std::size_t nodeCount, Sharing sharing)
            : entries_(nodeCount), sharing_(sharing) {}

        /// Forgets everything. Cheap, and only needed if the same cache is
        /// reused for a different world or pipeline. The scope frames need
        /// no forgetting: a scope never reads what an earlier one wrote.
        void reset() noexcept { forget(); }

        [[nodiscard]] Sharing sharing() const noexcept { return sharing_; }

        /// The most slots the memo grows to; it holds at most half as many
        /// values, and is forgotten whole and refilled past that.
        static constexpr std::size_t kMemoCapacity = MemoTable::kMaxCapacity;

        /// FOR TESTS. How many values the memo holds.
        [[nodiscard]] std::size_t memoSize() const noexcept { return memo_.size(); }

        /// FOR TESTS. Moves the memo's epoch forward to at least @p epoch,
        /// as though it had been forgotten that many times, never back. The
        /// epoch wraps after 2^32 forgettings, which no chunk comes near, and
        /// the wrap must forget the memo exactly as each forgetting does;
        /// this is how a test gets there.
        void advanceMemoEpoch(std::uint32_t epoch) noexcept { memo_.advanceEpoch(epoch); }

        /// FOR TESTS. How many times the memo's epoch has wrapped.
        [[nodiscard]] std::uint64_t memoEpochWraps() const noexcept { return memo_.wraps(); }

        /// FOR TESTS. Moves the scope frames' epoch counters forward to at
        /// least @p epoch, as though that many scopes had opened at every
        /// level of nesting — never back, so nothing written so far can be
        /// taken for the work of a scope still to come. A counter wraps
        /// after 2^32 scopes at one level, which no chunk comes near, and
        /// the wrap must forget its frame exactly as a fresh scope does;
        /// this is how a test gets there.
        void advanceScopeEpochs(std::uint32_t epoch) noexcept { scopes_.advanceEpochs(epoch); }

        /// FOR TESTS. How many times a scope frame's epoch counter has
        /// wrapped, so that a test of the wrap can show it happened.
        [[nodiscard]] std::uint64_t scopeEpochWraps() const noexcept { return scopes_.wraps(); }

    private:
        friend class Interpreter;

        /// How many distinct windows the memo tells apart before it is
        /// forgotten whole: a chunk reads through two (none, and its own).
        static constexpr std::size_t kMaxWindows = 8;

        struct Entry {
            /// The cell these corners belong to, by its lower corner.
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t z = 0;
            /// What the corners were read through (Interpreter's corner
            /// tag); compared only under Sharing::Lattice.
            std::uint32_t tag = 0;
            bool filled = false;
            /// Whether alongY holds these corners' four blends along y at
            /// the fraction whose bits are yFraction: what a BlockKernel
            /// blended last in this cell, kept for the next block of the
            /// same y — the same lerps of the same doubles. Cleared whenever
            /// the corners are refilled.
            bool alongYKnown = false;
            std::uint64_t yFraction = 0;
            std::array<double, 4> alongY{};
            std::array<double, 8> corners{};
        };

        /// A small number standing for @p window in memo keys: 0 for none,
        /// and the same number for every window with the same bounds until
        /// the cache is forgotten. A ninth distinct window forgets it first.
        [[nodiscard]] std::uint32_t windowId(const FlatCacheWindow* window) noexcept;

        /// Every corner, memo value and window number dropped.
        void forget() noexcept {
            for (Entry& entry : entries_) {
                entry.filled = false;
            }
            memo_.clear();
            windowCount_ = 0;
        }

        std::vector<Entry> entries_;
        ScopeArena scopes_;
        MemoTable memo_;
        std::array<FlatCacheWindow, kMaxWindows> windows_{};
        std::uint32_t windowCount_ = 0;
        Sharing sharing_ = Sharing::Lattice;
        /// A BlockKernel evaluation's registers: each written before it is
        /// read, so never cleared. Scratch, as the scope frames are.
        std::vector<double> kernelValues_;
        /// The last cell a BlockKernel found a point in (cellCorner), with
        /// the geometry it was found under: a point inside it is in it, and
        /// its corners passed cellCorner's check, so a kernel takes it
        /// without dividing again. Never a value, only where to find one.
        bool kernelCellKnown_ = false;
        CellGeometry kernelCellGeometry_{};
        Point kernelCell_{};
        /// interpolate()'s fractions for kernelCellGeometry_, by offset into
        /// the cell — offset / width and offset / height, the same
        /// divisions, made once per geometry rather than three a block —
        /// where both sizes are at most kKernelFractions; tables are
        /// unused otherwise. Never a value of their own.
        static constexpr std::size_t kKernelFractions = 16;
        bool kernelFractionTables_ = false;
        bool kernelGeometryKnown_ = false;
        std::array<double, kKernelFractions> kernelWidthFractions_{};
        std::array<double, kKernelFractions> kernelHeightFractions_{};
    };

    /// SPEC §4.1's compiled block program, for the part of a root that is
    /// evaluated at every block: the arithmetic from the root down to its
    /// `interpolated` nodes, lowered at construction to a straight list of
    /// ops over registers that evaluate(kernel, at, cache) runs instead of
    /// walking the graph.
    ///
    /// WHAT IT LOWERS. A root's block part is the root and every node
    /// reached from it without passing through an `interpolated`: there the
    /// blend of the argument's cell corners is an op, and the argument is
    /// evaluated at the corners by the interpreter, exactly as before. Every
    /// node of the block part must be one of: constant, the two blend states,
    /// abs, add, clamp, cube, half_negative, invert, max, min, mul,
    /// quarter_negative, range_choice, square, squeeze, y_clamped_gradient,
    /// interpolated, and the transparent caches (blend_density, cache_2d,
    /// cache_once, cache_all_in_cell); none may draw a refusal or keep a
    /// column memo; and the list may not pass kMaxOps ops or kMaxRegisters
    /// registers. Otherwise there is no kernel, and the caller evaluates the
    /// root as before (blockKernel).
    ///
    /// EXACT. The list is the walk evaluateNode makes, in its order:
    /// arguments before their node, first to last; range_choice tests its
    /// input and jumps over the branch it does not take, so only the taken
    /// one is evaluated. Each op computes its node's value with
    /// evaluateNode's own expression. An arithmetic node the block part
    /// reaches twice is lowered at each reach and so computed again where
    /// the walk read its memo — the same value from the same registers, and
    /// nothing besides. An `interpolated` reached twice is not: every reach
    /// shares one register and one bit of the point's mask, so it reads its
    /// corners once a point, as the walk's memo does — a second read could
    /// find the entry moved to another cell by a corner read in between,
    /// and refill it. An `interpolated` finds its cell
    /// with cellCorner at the first one evaluated at the point, where the
    /// walk first found it, and its corners in the CornerCache's entry,
    /// filled by the same code in the same order when the cell is not the
    /// entry's, so every corner is evaluated at the same moment it always
    /// was. No op of the block part can refuse (built only where none does),
    /// so the refusals are the corners' own, thrown where they always were.
    ///
    /// Immutable, and free of pointers into the interpreter: node indices
    /// only. Its scratch lives in the caller's CornerCache.
    class BlockKernel {
    public:
        /// The most ops a kernel's list holds.
        static constexpr std::size_t kMaxOps = 128;
        /// The most registers it writes.
        static constexpr std::size_t kMaxRegisters = 64;

        /// The root this kernel evaluates.
        [[nodiscard]] NodeIndex root() const noexcept { return root_; }

        /// How many ops its list holds.
        [[nodiscard]] std::size_t size() const noexcept { return ops_.size(); }

    private:
        friend class Interpreter;

        enum class Code : std::uint8_t {
            /// Registers: `target` from `first` and `second` as the node
            /// type's own expression; parameters as the node holds them.
            Add,
            Mul,
            Min,
            Max,
            Abs,
            Square,
            Cube,
            HalfNegative,
            QuarterNegative,
            Squeeze,
            Invert,
            Clamp,
            YClampedGradient,
            /// `target` is the blend of `interpolated`'s cell corners.
            Interpolated,
            /// The same, for a node the block part reaches more than once:
            /// read once a point, at the first reach that runs (`slot`'s bit).
            InterpolatedShared,
            /// `target` = `first`.
            Move,
            /// Continues at `jump` unless `first` is in range_choice's range.
            BranchOutOfRange,
            /// Continues at `jump`.
            Jump,
        };

        struct KernelOp {
            Code code = Code::Move;
            std::uint8_t target = 0;
            std::uint8_t first = 0;
            std::uint8_t second = 0;
            std::uint8_t jump = 0;
            /// For Interpolated, its node's bit in the point's mask.
            std::uint8_t slot = 0;
            /// For Interpolated, its argument's node.
            NodeIndex interpolated = 0;
            std::array<double, 4> parameters{};
        };

        /// One `interpolated` node of the block part: the register and the
        /// mask bit every reach of it shares.
        struct InterpolatedNode {
            NodeIndex node = 0;
            std::uint8_t target = 0;
            std::uint8_t slot = 0;
            /// Reached more than once: its ops are InterpolatedShared.
            bool shared = false;
        };

        /// A constant's register and value: written before the list runs,
        /// rather than by an op of it, since a constant has no order to keep.
        struct KernelConstant {
            std::uint8_t target = 0;
            double value = 0.0;
        };

        std::vector<KernelOp> ops_;
        std::vector<KernelConstant> constants_;
        std::vector<InterpolatedNode> interpolated_;
        std::size_t registers_ = 0;
        std::uint8_t result_ = 0;
        NodeIndex root_ = 0;
    };

    /// @p root's BlockKernel, or nothing when its block part holds a node the
    /// kernel does not lower (see BlockKernel). Refuses, as evaluate() does,
    /// a @p root that names no node.
    [[nodiscard]] std::optional<BlockKernel> blockKernel(NodeIndex root) const;

    /// evaluate(kernel.root(), at, cache), bit for bit, refusals included —
    /// through the kernel. @p kernel must be one this interpreter built.
    [[nodiscard]] double evaluate(const BlockKernel& kernel, Point at, CornerCache& cache) const;

    /// The value of @p root at @p at.
    ///
    /// Any int32 point is accepted, and what cannot be evaluated there is
    /// refused by EvalError, naming the node (SPEC §8) — among it an
    /// `interpolated` whose cell at @p at, or at a point the graph moves to
    /// (find_top_surface's scan), has a corner outside int32: within one
    /// cell's width or height of either end of an axis. Inside a chunk of
    /// kMaxChunkCoordinate's domain no x or z comes near one; y does only
    /// through find_top_surface. A @p root that names no node is refused
    /// the same way, by all three forms.
    [[nodiscard]] double evaluate(NodeIndex root, Point at) const;

    /// The same, reusing @p cache for the cell corners `interpolated` needs.
    /// Bit-for-bit identical to the two-argument form; only faster, and only
    /// when consecutive calls stay inside a cell.
    [[nodiscard]] double evaluate(NodeIndex root, Point at, CornerCache& cache) const;

    /// The same, with `flat_cache` relocating only inside @p window (see
    /// FlatCacheWindow), for the read @p context names (see ReadContext).
    /// Identical to the form above at every point whose column the window
    /// covers, for a `Block` read.
    [[nodiscard]] double evaluate(NodeIndex root, Point at, CornerCache& cache,
                                  const FlatCacheWindow& window,
                                  ReadContext context = ReadContext::Block) const;

    /// How many entries a CornerCache for this interpreter needs.
    [[nodiscard]] std::size_t cacheSize() const noexcept;

    /// Why this interpreter cannot evaluate @p type, or nothing if it can.
    /// Depends on the instance, not only on the type: a cell lattice is what
    /// makes `interpolated` meaningful. Public so that a caller can report
    /// the whole set up front rather than discovering them one refusal at a
    /// time.
    [[nodiscard]] std::optional<std::string_view> unevaluableReason(NodeType type) const noexcept;

    /// The cell lattice this interpreter samples on, if it has one.
    [[nodiscard]] const std::optional<CellGeometry>& cells() const noexcept { return cells_; }

    /// Whether @p index has the same value everywhere in a column — which is
    /// what makes caching it on (x, z) alone sound. Conservative: a node it
    /// cannot prove column-invariant is reported as varying.
    ///
    /// The rule, by type, with no default: constants, the two blend states,
    /// `end_islands`, `shift_a` and `shift_b` (which pin y) are invariant;
    /// `noise` is when its y_scale is zero, `shifted_noise` when that holds
    /// and its three shifts are; `spline` when its coordinate and every
    /// nested spline's are; the pure arithmetic, `range_choice`, the
    /// transparent caches (`cache_2d`, `cache_once`, `blend_density`),
    /// `flat_cache` and `find_top_surface` when all their arguments are;
    /// `interpolated` and `cache_all_in_cell` when the interpreter has a
    /// lattice and their argument is — though the lattice is set after this
    /// is computed, so today they are always reported varying; and
    /// `y_clamped_gradient`, `shift`, `old_blended_noise`,
    /// `weird_scaled_sampler` and `slide` never are. A node whose noise is
    /// written inline is reported varying. Exact to the bit, signed zeros
    /// included: y reaches an invariant noise only as y * 0.0, a zero of
    /// either sign, which every octave adds to its origin (never -0.0) or a
    /// shift adds to, before anything reads it.
    [[nodiscard]] bool isColumnInvariant(NodeIndex index) const;

private:
    /// One point's evaluation. Every node in a graph is a pure function of
    /// the point, so a node reached twice — `shift_x` is referenced five
    /// times by vanilla's overworld — is computed once and remembered. This
    /// is not the compiled program SPEC §4.1 asks for; it is what keeps the
    /// interpreter from being exponential in the depth of a spline tree.
    /// Its memo lives in a frame of a ScopeArena.
    class Scope;

    /// What every scope one top-level call opens shares: where scopes lease
    /// their frames, the caller's CornerCache, the window and the read
    /// context, and which of the cache's memos the call may use. Fixed for
    /// the whole call: a scope opened for another point inherits all of it.
    struct Call;

    /// weird_scaled_sampler's `rarity_value_mapper`, resolved from its name
    /// once rather than compared as a string on every sample.
    enum class Rarity : std::uint8_t {
        /// Not a weird_scaled_sampler.
        None,
        Type1,
        Type2,
        /// A name rarityValueMapper does not know, which it refuses — by
        /// name, at the sample, as it always did. The graph allows only the
        /// two above, so nothing reaches this.
        Unknown,
    };

    /// One node as evaluateNode reads it, copied out of the graph's Node at
    /// construction: its type and every operand, flat, instead of behind
    /// the two vectors and the optionals a Node holds; and the facts about
    /// it that are fixed once the interpreter is built.
    ///
    /// Exact by construction: the same doubles and indices, read in the
    /// same order. A node carries exactly its type's counts of arguments
    /// and parameters (the graph refuses any other shape), and no type has
    /// more than kMaxNodeArguments and kMaxNodeParameters.
    struct Op {
        NodeType type = NodeType::Constant;
        /// Whether refuseIfUnevaluable() throws for this node, decided once
        /// both constructors are done (settleRefusals). It depends on the
        /// node's type and noise fields, on the registry the interpreter
        /// bound and on its cell lattice, none of which a visit or
        /// substitute() changes; so a visit that finds it set throws the
        /// same message refuseIfUnevaluable always threw, at the same visit.
        bool refuses = false;
        /// Whether the node owns a spline; `spline` names it when it does.
        bool hasSpline = false;
        /// Whether a `flat_cache` is reachable from the node, through its
        /// arguments and splines: the one way the FlatCacheWindow a value
        /// is read through can change it. An `interpolated` argument with
        /// this set keeps its corners per window; one without keeps one
        /// set for every window, since every window gives it the same.
        bool readsWindow = false;
        /// Whether a CornerCache keeps the node's value per column
        /// (CornerCache's column memo): the node is column-invariant
        /// (isColumnInvariant); nothing it reaches reads the cell structure
        /// or the caller's corners, or is a type an UnsettledSubstitutions
        /// can replace — no `interpolated`, `cache_all_in_cell`,
        /// `find_top_surface`, `old_blended_noise` or
        /// `weird_scaled_sampler` — so its value is a function of the
        /// column, the window and the read context alone; and it reaches a
        /// noise or a spline, so that keeping it saves more than it costs.
        bool columnMemo = false;
        Rarity rarity = Rarity::None;
        SplineIndex spline = 0;
        std::array<NodeIndex, kMaxNodeArguments> arguments{};
        std::array<double, kMaxNodeParameters> parameters{};
        /// The noise the node samples, resolved once; null for a node that
        /// samples none, and for a named noise the registry does not hold
        /// (which refuseIfUnevaluable refuses).
        const noise::NormalNoise* noise = nullptr;
    };

    /// Why refuseIfUnevaluable() refuses a node, in the order it checks.
    enum class Refusal : std::uint8_t { None, Type, UnbuiltNoise, InlineNoise };

    /// Which refusal, if any, the node at @p index draws. The one definition
    /// both refuseIfUnevaluable() and Op::refuses are read from.
    [[nodiscard]] Refusal refusalOf(NodeIndex index) const noexcept;

    /// Sets every Op::refuses from refusalOf(). Run last by each constructor,
    /// since the cell lattice is part of the answer and the one with a
    /// lattice sets it after delegating.
    void settleRefusals() noexcept;

    /// Sets every Op::readsWindow and Op::columnMemo, from what each node
    /// reaches and from column invariance, which must be settled first.
    void settleReach(const Graph& graph);

    /// @p root refused by name unless it names a node of the graph.
    void requireNode(NodeIndex root) const;

    [[nodiscard]] double evaluateNode(Scope& scope, NodeIndex index) const;

    /// The cases of evaluateNode that hold more than a few doubles, kept out
    /// of its frame, which a graph pays once per level of its depth (SPEC
    /// §8). Each is that case exactly.
    [[nodiscard]] double oldBlendedNoise(NodeIndex index, const Op& op, Point at) const;
    [[nodiscard]] double endIslands(Point at) const;
    [[nodiscard]] double findTopSurface(Scope& scope, const Op& op) const;
    [[nodiscard]] double weirdScaledSampler(Scope& scope, NodeIndex index, const Op& op) const;
    [[nodiscard]] double flatCache(Scope& scope, const Op& op) const;

    /// Splines are evaluated in float, not double: their knot locations,
    /// derivatives and values are floats in vanilla, and the coordinate is
    /// narrowed to float before it is used. Widening the arithmetic would
    /// change the last bits of every terrain offset in the world.
    [[nodiscard]] float evaluateSpline(Scope& scope, SplineIndex index) const;

    [[nodiscard]] const noise::NormalNoise& noiseFor(NodeIndex index) const;

    /// `noiseFor(index).sample(x, y, z)`, refused by name — EvalError naming
    /// the node, its noise and the block @p at — where the noise is not
    /// defined (noise::NormalNoise::accepts): a scale, a shift or a block
    /// coordinate that carries some octave's lattice out of int32, or a NaN.
    /// Vanilla's own `jagged` gets there inside the world border (SPEC §8,
    /// "Inputs that would reach undefined behaviour").
    [[nodiscard]] double sampleNoise(NodeIndex index, const Point& at, double x, double y,
                                     double z) const;

    /// The blended noise built for @p index. Throws rather than returning a
    /// wrong value if none was built, which can only happen if the graph
    /// changed under the interpreter.
    [[nodiscard]] const noise::BlendedNoise& blendedFor(NodeIndex index) const;

    /// Refuses the node at @p index, by name and with a reason, if this
    /// interpreter cannot evaluate it — because of its type, because its
    /// noise field carried parameters inline, or because the named noise it
    /// samples was never built into the registry. Both requireEvaluable()
    /// and evaluate() go through it, so a refusal reads the same whichever
    /// asked.
    ///
    /// Takes an index rather than the node, because the third of those three
    /// is a fact about this interpreter's binding table and not about the
    /// node alone.
    void refuseIfUnevaluable(NodeIndex index) const;

    void requireEvaluableNode(NodeIndex index, std::vector<char>& seen) const;
    void requireEvaluableSpline(SplineIndex index, std::vector<char>& seen) const;

    /// Trilinear interpolation over the eight corners of @p at's cell.
    /// Vanilla computes each corner once per cell and reuses it across every
    /// block in that cell; without a CornerCache this recomputes them per
    /// point, which is the same value at eight times the cost — the density
    /// function is a pure function of position. A cache makes that once per
    /// cell again (SPEC §4.1), and its lattice once per corner.
    [[nodiscard]] double interpolate(NodeIndex argument, const Scope& scope) const;

    /// @p argument at the cell corner @p corner, from the cache's lattice
    /// under @p tag, or evaluated there (in a scope on @p call keeping cell
    /// corners as @p cells says) and kept. @p call's cache is set.
    [[nodiscard]] double sharedCorner(NodeIndex argument, const Call& call, bool cells,
                                      Point corner, std::uint32_t tag) const;

    /// Fills @p entry with @p argument's eight corners for the cell whose
    /// lower corner is @p lower, kept under @p tag: through the cache's
    /// lattice when @p call keeps a memo, each evaluated afresh otherwise, in
    /// kCornerOrder either way. Corner scopes open on @p call, keeping cell
    /// corners as @p cells says. What interpolate() and a BlockKernel both do
    /// when the cell is not the entry's.
    void fillCell(NodeIndex argument, const Call& call, bool cells, CornerCache::Entry& entry,
                  Point lower, std::uint32_t tag) const;

    /// Lowers @p index, reached at @p depth, onto @p kernel's list, and
    /// returns the register holding its value, or -1 where it does not lower.
    [[nodiscard]] std::int32_t lowerBlock(BlockKernel& kernel, NodeIndex index,
                                          std::size_t depth) const;

    /// The memo key of node @p index's value in @p scope's column (see
    /// Op::columnMemo).
    [[nodiscard]] static MemoTable::Key columnKey(NodeIndex index, const Scope& scope) noexcept;

    /// The Call a top-level evaluation through @p cache makes.
    [[nodiscard]] Call callThrough(CornerCache& cache, const FlatCacheWindow* window,
                                   ReadContext context) const noexcept;

    const Graph* graph_;
    std::optional<CellGeometry> cells_;
    /// Every node as evaluateNode reads it (Op), indexed by node, with the
    /// noise each samples resolved once.
    std::vector<Op> ops_;
    /// The blended noise each `old_blended_noise` node needs, built once at
    /// construction, indexed by node. Building one draws forty Perlin
    /// permutations, which is not something to do per sample; and the noise is
    /// a function of the world seed and the node's own five parameters, both
    /// fixed once the pipeline is compiled.
    std::vector<std::optional<noise::BlendedNoise>> blendedOf_;
    /// The End island field, built once if any node needs it. It carries no
    /// per-node parameters at all — every `end_islands` node in a graph is
    /// the same function — so one per interpreter is one per graph.
    std::optional<noise::EndIslands> endIslands_;
    /// Column invariance per node, computed once at construction rather than
    /// memoised on demand: an interpreter is shared across threads and const
    /// has to mean const (SPEC §4.1).
    std::vector<char> columnInvariant_;
    std::vector<char> splineColumnInvariant_;
    UnsettledSubstitutions substitutions_;
};

} // namespace stratum::density
