// Stratum — the multi-noise biome parameter table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Which biome sits at a point is decided by six climate values and a table
// that maps regions of that six-dimensional space to biomes. This is the
// table, and the search over it.
//
// WHERE THE NUMBERS COME FROM, because it is not where you would expect.
// At 1.21.11 the pack ships
// `worldgen/multi_noise_biome_source_parameter_list/overworld.json` as
// nothing but `{"preset": "minecraft:overworld"}` — the table itself is
// compiled into the jar and is not data. The server will however dump it, on
// request, through its own data generator, and `tools/fetch-vanilla` asks it
// to. CLAUDE.md permits the observed output of the vanilla server where it
// forbids its code, and this is squarely that: 7593 entries over 54 biomes
// for the overworld, written by Mojang's own serialiser.
//
// It is Mojang-derived and therefore never committed (SPEC §12). Without the
// fixtures, everything here has nothing to load and the tests that need it
// say so.
//
// ADAPTED (CLAUDE.md, NOTICE): quantizeCoord below is adapted from cubiomes
// (https://github.com/Cubitect/cubiomes, MIT License, Copyright (c) 2020
// Cubitect), biomenoise.c `sampleBiomeNoise`, read at
// e61f90580cbdd883214a8054670dacae655e59c0: `(int64_t)(10000.0F*t)` over its
// `float` climate values — the ten thousand steps a unit, the narrowing to
// float, the float product, the truncation. No other source on record gives
// the scale or the float step; the goldens measure the truncation against
// flooring and rounding, and score truncation through float and through
// double alike (SPEC §11). THIS PROJECT'S OWN: quantising the table's bounds
// by the same function at load, where cubiomes carries its search tree's
// bounds precomputed, and the bound that keeps the conversion defined
// (kMaxClimateSample).

#pragma once

#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::biome {

/// Raised when a parameter list cannot be read. Names the entry and the axis,
/// because a table with seven thousand rows needs to say which one is wrong.
class ParameterError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The search does not run in the reals. Every climate value and every
/// bound is first mapped to a fixed-point integer — ten thousand steps per
/// unit, truncated through `float` — and the distances are integer
/// distances. This is not an optimisation, it is part of the answer: two
/// entries whose real-valued fitnesses differ in the ninth decimal place are
/// *exactly equal* here, and which of them wins is then decided by the
/// tie-break rather than by that difference. SPEC §11 records the
/// measurement that established it.
///
/// The cast through `float` before the multiply is deliberate and is the
/// reason this is not simply `llround(value * 10000)`. It is cubiomes'
/// (`sampleBiomeNoise`; see the header and NOTICE): the goldens measure the
/// truncation, not the float step, which they score as they score a double.
///
/// PRECONDITION: |@p value| <= kMaxClimateSample, NaN excluded — beyond
/// about 9.2e14 the conversion to int64 is undefined behaviour (SPEC §5.7).
/// Both kinds of input are held to it before they get here: a table's bounds
/// by `ParameterList::fromEntries`, which refuses anything outside the
/// schema's [-2, 2], and a sample by `ParameterList::find`, which refuses by
/// name a climate reading outside ±kMaxClimateSample.
[[nodiscard]] constexpr std::int64_t quantizeCoord(double value) noexcept {
    return static_cast<std::int64_t>(static_cast<float>(value) * 10000.0F);
}

/// The widest climate reading the search is defined for. A router entry is
/// an arbitrary density function, so a datapack can hand the search any
/// double — `cube(1e6)` is 1e18 — and two things break past a bound: the
/// quantising conversion above (at about 9.2e14), and, well before it,
/// QuantizedPoint::fitness's int64 sum of squares (one axis at about 3e5, all
/// six at about 1.2e5). At 1e5 each distance is at most 1e9 + 2e4, the sum of
/// six squares and the offset's under 6.1e18, inside int64 with room to spare.
/// Vanilla's own readings stay within a few units. What the server does with
/// a reading past this is unmeasured, so this build refuses it rather than
/// pick a reading (SPEC §8).
inline constexpr double kMaxClimateSample = 1.0e5;

/// The schema's range for a climate bound (vanilla-mcdoc
/// `ClimateParameter`: float @ -2..2) and for an entry's offset
/// (`offset`: float @ 0..1). The server's own dumped tables lie well inside
/// both: [-1.2, 1.1], offsets up to 0.375.
inline constexpr double kClimateBoundLimit = 2.0;
inline constexpr double kMaxClimateOffset = 1.0;

/// One axis of one entry. Vanilla writes either a `[min, max]` pair or a
/// single number, which is a range of zero width rather than a special case.
struct Parameter {
    double min = 0.0;
    double max = 0.0;

    /// Exact equality, spelled without a float `==` because the project
    /// builds with -Wfloat-equal (see perlin.cpp's `isZero`). A defaulted
    /// `operator==` here is compiler-generated float `==`: GCC lets that
    /// through, Clang does not, and nothing instantiated it until
    /// vanilla_legacy_goldens_biome_test.cpp compared parameter-list entries
    /// — which is how it reddened every Clang and AppleClang leg at f792e64
    /// while GCC and MSVC stayed green.
    ///
    /// For every value JSON can yield (finite, never NaN), `!(a < b) &&
    /// !(b < a)` is IEEE `a == b` to the bit, -0.0 == 0.0 included. These are
    /// parsed constants compared for entry identity, not arithmetic results
    /// compared with a tolerance — the exactness is the intent.
    [[nodiscard]] bool operator==(const Parameter& other) const noexcept {
        return !(min < other.min) && !(other.min < min) && !(max < other.max) && !(other.max < max);
    }
};

/// The six climate values a point in the world has. `depth` is a function of
/// height as well as position, which is why a biome can change up a column
/// without anything horizontal changing.
struct ClimateSample {
    double temperature = 0.0;
    double humidity = 0.0;
    double continentalness = 0.0;
    double erosion = 0.0;
    double depth = 0.0;
    double weirdness = 0.0;
};

/// One row of the table: six ranges, plus an offset that is not a climate
/// the world has. The offset is a constant added to every distance, so a
/// biome carrying one is chosen only where nothing else is close.
struct ParameterPoint {
    Parameter temperature;
    Parameter humidity;
    Parameter continentalness;
    Parameter erosion;
    Parameter depth;
    Parameter weirdness;
    double offset = 0.0;

    /// Vanilla's fitness for @p sample. Convenience over QuantizedPoint —
    /// correct, but it quantises this entry on every call, so the search
    /// itself uses the precomputed form instead.
    [[nodiscard]] std::int64_t fitness(const ClimateSample& sample) const noexcept;

    [[nodiscard]] bool operator==(const ParameterPoint& other) const = default;
};

/// A climate sample in the space the comparison actually happens in.
struct QuantizedSample {
    std::int64_t temperature = 0;
    std::int64_t humidity = 0;
    std::int64_t continentalness = 0;
    std::int64_t erosion = 0;
    std::int64_t depth = 0;
    std::int64_t weirdness = 0;

    [[nodiscard]] static QuantizedSample of(const ClimateSample& sample) noexcept;

    [[nodiscard]] bool operator==(const QuantizedSample& other) const = default;
};

/// A table row in that same space. Building one costs fourteen conversions,
/// so the list builds them once at load rather than seven thousand times per
/// lookup.
struct QuantizedPoint {
    /// Indexed rather than named, in the order ParameterPoint declares:
    /// temperature, humidity, continentalness, erosion, depth, weirdness.
    std::array<std::int64_t, 6> min{};
    std::array<std::int64_t, 6> max{};
    std::int64_t offset = 0;

    [[nodiscard]] static QuantizedPoint of(const ParameterPoint& point) noexcept;

    /// The squared distance on each axis, summed, plus the offset squared.
    /// Squared throughout: only the ordering matters, and a square root
    /// would put rounding back into a comparison that has been made exact.
    ///
    /// Inside int64 for every input the list lets through, and no further: a
    /// bound is within ±2 (20000 quantised) and the offset within [0, 1]
    /// (fromEntries refuses anything else), and a sample within
    /// ±kMaxClimateSample (1e9 quantised; `find` refuses anything else). So a
    /// distance is at most 1 000 020 000 and the seven terms sum to under
    /// 6.1e18. An earlier version of this comment called ±2 against ∓2 the
    /// widest input; the SAMPLE was never bounded, and a router temperature
    /// of the constant 1e6 — schema-legal — overflowed this sum.
    [[nodiscard]] std::int64_t fitness(const QuantizedSample& sample) const noexcept;

    [[nodiscard]] bool operator==(const QuantizedPoint& other) const = default;
};

struct Entry {
    ParameterPoint parameters;
    data::ResourceLocation biome{"minecraft", "plains"};
};

/// The widest a quantised sample coordinate can be: kMaxClimateSample through
/// quantizeCoord. `ParameterList::findRow` over a QuantizedSample refuses
/// anything past it, for the same overflow reason.
inline constexpr std::int64_t kMaxQuantizedSample = quantizeCoord(kMaxClimateSample);
static_assert(kMaxQuantizedSample == 1'000'000'000);

/// How much of the search tree queries looked at. The tests' and the bench's
/// window into the search, never consulted by it: what a query returns does
/// not depend on it, and the plain `find` does not count at all.
struct SearchCounts {
    /// Table rows whose fitness was computed.
    std::uint64_t rows = 0;
    /// Tree nodes whose lower bound was computed.
    std::uint64_t nodes = 0;
};

/// What the search tree costs to hold: its size, built once per list.
struct SearchTreeShape {
    std::size_t nodes = 0;
    std::size_t leaves = 0;
    /// Edges from the root to the deepest leaf; 0 for a list one leaf holds.
    std::size_t depth = 0;
    /// The tree's own arrays (nodes and their rows), not the entries.
    std::size_t bytes = 0;
};

namespace detail {

/// One axis-aligned box in the space the search runs in, axes in
/// QuantizedPoint's order. For a table row it is that row's ranges and
/// offset, `row` its index in the list. For a tree node it is the smallest
/// box holding every row under the node, `offset` the smallest offset under
/// it and `row` the LARGEST row index under it. One function scores both,
/// which is what makes a node's score a lower bound on every row's below it.
///
/// int32 rather than QuantizedPoint's int64, to halve what a query reads:
/// `ParameterList::fromEntries` holds every bound to the schema's ±2 (±20000
/// quantised) and every offset to [0, 1] ([0, 10000]), and a list to a
/// million rows, so each field fits with room to spare. The arithmetic over
/// them is int64, exactly QuantizedPoint::fitness's.
struct SearchBox {
    std::array<std::int32_t, 6> min{};
    std::array<std::int32_t, 6> max{};
    std::int32_t offset = 0;
    std::int32_t row = 0;
};

/// A node of the search tree. Children are allocated in pairs, so an inner
/// node names only its left child.
struct SearchNode {
    SearchBox box;
    /// A leaf: its first row in the tree's row array. An inner node: its left
    /// child, the right being `first + 1`.
    std::uint32_t first = 0;
    /// A leaf: how many rows it holds, at least one. An inner node: zero.
    std::uint32_t count = 0;
};

} // namespace detail

/// A dimension's table, and the search over it.
class ParameterList {
public:
    /// Reads the shape the server's data generator writes. @p id names the
    /// list in any error raised.
    [[nodiscard]] static ParameterList fromJson(const nlohmann::json& json,
                                                const data::ResourceLocation& id);

    /// A list from entries already read — a frozen pipeline's (SPEC §6).
    /// Checks what `fromJson` checks of the numbers themselves: no more than
    /// a dimension's worth of entries, no axis with its minimum above its
    /// maximum, every bound inside the schema's [-2, 2] and every offset
    /// inside its [0, 1] — NaN and infinities refused with them, which a
    /// resealed blob can carry and JSON's `1e999` can too. `fromJson` ends
    /// here, so the two build the same list and refuse the same input.
    [[nodiscard]] static ParameterList fromEntries(std::vector<Entry> entries,
                                                   const data::ResourceLocation& id);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// The biome whose entry fits @p sample best.
    ///
    /// Ties go to the **later** entry. Ties are common rather than exotic —
    /// the comparison is over quantised integers, so entries that differ
    /// only in the ninth decimal place land on the same value — which makes
    /// this rule load-bearing rather than a detail. Vanilla does not scan a
    /// list at all; it searches a tree built from one, and which leaf a tie
    /// resolves to is a property of that tree's shape. "Later row wins"
    /// reproduces every tie observed across 98304 cells and four seeds, but
    /// it is a measured match and not a derivation of the tree — SPEC §11
    /// records that distinction, and a counterexample would be a finding
    /// about the tree rather than a bug in the arithmetic.
    ///
    /// That counterexample now exists, and this rule is a PROXY. Over every
    /// fourth chunk of all eight golden overworld regions (32768 cells,
    /// tests/conformance/vanilla_biome_tie_break_test.cpp) twenty cells in
    /// five columns are genuine ties — equal minimum fitness, equidistant on
    /// every axis, the sample sitting on a shared quantised bound — that
    /// vanilla resolves to the EARLIER row. Across both samples, 104 ties
    /// decide a biome: "later" is right on 84 and "earlier" on 20, each rule
    /// refuted by the other sample, and every tie is a pair, so the two
    /// positional rules exhaust what a list order can express. All 104 are
    /// consistent with one total order on rows that is not the list's —
    /// the shape of a tree's leaf order, which the parameter table does not
    /// carry and this project may not read from source. "Later" stays
    /// because inverting it is strictly worse; the twenty are attributed,
    /// not fixed, and the [biome] cases now state the measured extent.
    ///
    /// HOW IT IS FOUND, which is not how it is defined. The answer is defined
    /// as the linear scan `findRowLinear` does: every row scored, the
    /// lexicographic minimum of (fitness, -row) kept, so the least fitness
    /// and, among rows tied at it, the last. Scoring all 7593 of the
    /// overworld's rows was nearly half of what a chunk cost, so `find`
    /// instead searches a tree of axis-aligned boxes over the quantised rows,
    /// built once by `fromEntries` (a frozen pipeline's thaw included) and
    /// never changed after. A node's box holds every row under it, so its
    /// score — the same int64 arithmetic as a row's fitness, over the box,
    /// with the smallest offset under it — is never more than any of theirs.
    /// The search skips a node only when that bound is ABOVE the best fitness
    /// so far, or equal to it with every row under the node earlier than the
    /// best row so far — rows that could at most tie and would then lose the
    /// tie. Every other row is scored, and the best is kept by the same
    /// strict order the scan keeps, so the row is the scan's row, the tie
    /// rule above included, whatever the tree's shape: the shape decides only
    /// how many rows are scored. It is not vanilla's tree — that one's leaf
    /// order would BE the tie-break — and it cannot move a biome.
    ///
    /// Throws ParameterError on an empty list rather than inventing a biome:
    /// there is no sensible default, and a world full of one wrong biome is
    /// worse than a refusal. And throws ParameterError, naming the climate
    /// value and its axis, for a sample outside ±kMaxClimateSample or NaN,
    /// where the search's own arithmetic is undefined (SPEC §8).
    [[nodiscard]] const data::ResourceLocation& find(const ClimateSample& sample) const;

    /// The row `find` takes, as an index into `entries()`. Refuses what
    /// `find` refuses.
    [[nodiscard]] std::size_t findRow(const ClimateSample& sample) const;

    /// The same search from a sample already quantised. Refuses an empty
    /// list, and a coordinate outside ±kMaxQuantizedSample by axis name.
    [[nodiscard]] std::size_t findRow(const QuantizedSample& sample) const;

    /// `findRow`, counting what it looked at into @p counts (added to, not
    /// reset). The same search and the same row; for the tests and the bench.
    [[nodiscard]] std::size_t findRow(const QuantizedSample& sample, SearchCounts& counts) const;

    /// What `findRowFrom` takes for "no warm start": any value at or past
    /// `size()` is one, this the conventional spelling.
    static constexpr std::size_t kNoHint = static_cast<std::size_t>(-1);

    /// `findRow`, warm-started from @p hint: the same row for every hint,
    /// only reached sooner when the hint is near it. A run of queries over
    /// neighbouring positions — a chunk's quarts, whose climate moves little
    /// from one to the next — passes each answer back in as the next hint.
    /// The hint is the CALLER's state, held per call or per task, never in
    /// the list: a list is shared read-only between threads.
    ///
    /// WHY THE ROW CANNOT DEPEND ON THE HINT. The search keeps the best row
    /// under the scan's strict total order — less fitness, or equal fitness
    /// and a later row — and a hinted row is simply scored before the tree is
    /// entered, as if it were the first row met. A node is skipped only when
    /// its bound is ABOVE the best fitness so far, or EQUAL to it with every
    /// row under it EARLIER than the best row so far: each row skipped then
    /// either scores more than the best or ties it and loses the tie. A later
    /// row at an equal fitness is never skipped (the `<` on rows is strict),
    /// so a hint at a tied earlier row cannot hide the later one the scan
    /// keeps, and a hint at the tied later row is itself the scan's answer.
    /// The best only ever improves, so every skipped row is worse than the
    /// final best, which is therefore the order's minimum over every row: the
    /// scan's row. A hint at or past `size()` is no hint (kNoHint).
    ///
    /// Refuses what `findRow` refuses, and before the hint is looked at.
    [[nodiscard]] std::size_t findRowFrom(const ClimateSample& sample, std::size_t hint) const;
    [[nodiscard]] std::size_t findRowFrom(const QuantizedSample& sample, std::size_t hint) const;
    /// `findRowFrom`, counting into @p counts as `findRow` does; the hinted
    /// row's own scoring counts as one row.
    [[nodiscard]] std::size_t findRowFrom(const QuantizedSample& sample, std::size_t hint,
                                          SearchCounts& counts) const;

    /// The definition `find` is held to: every row scored in list order,
    /// `<=` keeping the later of a tie. Kept as the reference the tree's
    /// answers are tested against, row index for row index; costs a scan of
    /// the whole list per call. Refuses what the matching `findRow` refuses.
    [[nodiscard]] std::size_t findRowLinear(const ClimateSample& sample) const;
    [[nodiscard]] std::size_t findRowLinear(const QuantizedSample& sample) const;

    /// The tree's size and what it costs to hold.
    [[nodiscard]] SearchTreeShape searchTreeShape() const noexcept;

    /// Checks the tree is one the search may trust: every row in exactly one
    /// leaf, carrying exactly the bounds and offset the list quantises to,
    /// and every node's box, offset and largest row holding each child's —
    /// which makes every node's score a lower bound on every row's below it.
    /// Empty when sound; otherwise the first defect, named.
    [[nodiscard]] std::string auditSearchTree() const;

private:
    /// The deepest a tree is built (edges, root to leaf), so that a query's
    /// pending stack is a fixed array on the call stack and a query
    /// allocates nothing. The build guarantees it: see biome_parameter_list.cpp.
    static constexpr std::size_t kMaxSearchDepth = 64;

    void buildSearchTree();

    /// The tree search, warm-started from @p hint when it names a row (see
    /// findRowFrom; kNoHint for none).
    template<bool Counting>
    [[nodiscard]] std::size_t searchTree(const QuantizedSample& sample, std::size_t hint,
                                         SearchCounts* counts) const noexcept;

    [[nodiscard]] std::size_t scanLinear(const QuantizedSample& sample) const noexcept;

    std::vector<Entry> entries_;
    /// entries_[i] in the space the search runs in. Kept parallel rather
    /// than inside Entry so that Entry stays the table as it was written.
    /// The linear reference scores these, independently of the tree.
    std::vector<QuantizedPoint> quantized_;
    /// The search tree: nodes, root first, and the rows its leaves hold,
    /// grouped by leaf. Built once by fromEntries and const after.
    std::vector<detail::SearchNode> searchNodes_;
    std::vector<detail::SearchBox> searchRows_;
    std::size_t searchDepth_ = 0;
};

} // namespace stratum::biome
