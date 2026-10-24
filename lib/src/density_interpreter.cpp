// Stratum — evaluating a resolved density function graph, one point at a time.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ADAPTED (CLAUDE.md, NOTICE): Interpreter::evaluateSpline follows cubiomes
// (https://github.com/Cubitect/cubiomes, MIT License, Copyright (c) 2020
// Cubitect), biomenoise.c `getSpline`, read at
// e61f90580cbdd883214a8054670dacae655e59c0: the knot search, the straight
// line off either end, and the cubic written as
// `lerp(k, n, o) + k * (1 - k) * lerp(k, p, q)` with its two tangent terms.
// Recorded as an adaptation because it cannot be shown to be independent of
// that function: it arrived in the same commit as the explicit copy in
// tools/vectors/climate_vectors.c and matches it term for term. What is this
// project's own is the float-throughout evaluation (SPEC §11).

#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/noise/perlin.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace stratum::density {

namespace {

/// Zero, either sign of it. The project builds with -Wfloat-equal, and a
/// scale that is exactly zero is a structural fact about a node rather than
/// an approximate comparison.
[[nodiscard]] bool isZero(double value) noexcept {
    return (std::bit_cast<std::uint64_t>(value) & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0U;
}

/// A double as an error message spells it: every digit it carries.
[[nodiscard]] std::string describe(double value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}

/// A block position as an error message spells it.
[[nodiscard]] std::string describe(const Point& at) {
    return "(" + std::to_string(at.x) + ", " + std::to_string(at.y) + ", " + std::to_string(at.z) +
           ")";
}

/// Vanilla's lerp, in whatever type it is instantiated with: `from` plus a
/// fraction of the span, not the `(1-t)*a + t*b` form. The two disagree in
/// their last bits, and the spline is float, where last bits are close to
/// the surface.
template<typename T>
[[nodiscard]] constexpr T lerp(T part, T from, T to) noexcept {
    return from + (part * (to - from));
}

/// Vanilla's clampedMap: the fraction is computed first and the ends are
/// returned exactly when it leaves [0, 1], rather than clamping the fraction
/// and interpolating — which would return `from + 1 * (to - from)` at the
/// top end, and that is not always bit-identical to `to`.
[[nodiscard]] double clampedMap(double value, double fromIn, double toIn, double fromOut,
                                double toOut) noexcept {
    const double part = (value - fromIn) / (toIn - fromIn);
    if (!(part > 0.0)) {
        return fromOut;
    }
    if (part >= 1.0) {
        return toOut;
    }
    return lerp(part, fromOut, toOut);
}

/// The corner of the 4x4 column a block sits in. flat_cache samples there,
/// and floorDiv is the point: at negative coordinates a truncating division
/// would round towards zero and pick the wrong column.
[[nodiscard]] std::int32_t columnCorner(std::int32_t coordinate) noexcept {
    return javamath::floorDiv(coordinate, 4) * 4;
}

// The refusals evaluation makes, each built here rather than in the frame
// that finds it: those frames are on the stack once per level of a graph's
// depth, and a message's temporaries would be too.

[[noreturn]] void refuseUnevaluated(const NodeType type) {
    throw EvalError("'" + std::string(nodeTypeName(type)) + "' has no evaluation in this build");
}

[[noreturn]] void refuseCellHeight(const std::int32_t cellHeight) {
    throw EvalError("'minecraft:find_top_surface' has cell_height " + std::to_string(cellHeight) +
                    ", which would never finish scanning; it must be positive");
}

[[noreturn]] void refuseUpperBound(const double upperBound, const Point& at) {
    throw EvalError("'minecraft:find_top_surface' reads upper_bound " + describe(upperBound) +
                    " at block " + describe(at) +
                    ", whose floor is not a 32-bit integer; this build refuses it rather than "
                    "guess where the server's scan starts");
}

[[noreturn]] void refuseLatticePoint(const double upperBound, const Point& at,
                                     const std::int64_t start) {
    throw EvalError("'minecraft:find_top_surface' reads upper_bound " + describe(upperBound) +
                    " at block " + describe(at) + ", whose lattice point below it, " +
                    std::to_string(start) + ", is not a 32-bit integer; this build refuses it");
}

[[noreturn]] void refuseScan(const std::int64_t steps, const std::int32_t start,
                             const std::int32_t lowerBound) {
    throw EvalError("'minecraft:find_top_surface' would scan " + std::to_string(steps) +
                    " steps from " + std::to_string(start) + " down to " +
                    std::to_string(lowerBound) + "; that is not a column, it is a hang");
}

[[noreturn]] void refuseCell(const Point& at, const CellGeometry& cells, const std::int64_t x0,
                             const std::int64_t y0, const std::int64_t z0) {
    throw EvalError("'minecraft:interpolated' at block " + describe(at) + " blends the " +
                    std::to_string(cells.width) + "x" + std::to_string(cells.height) +
                    " cell from (" + std::to_string(x0) + ", " + std::to_string(y0) + ", " +
                    std::to_string(z0) +
                    "), whose corners are not all 32-bit integers; this build refuses it rather "
                    "than wrap them");
}

/// Whether a cell from @p lower, @p size long, has both its ends in int32.
[[nodiscard]] constexpr bool cellFits(const std::int64_t lower, const std::int32_t size) noexcept {
    return lower >= std::numeric_limits<std::int32_t>::min() &&
           lower + size <= std::numeric_limits<std::int32_t>::max();
}

/// The lower corner of the cell @p at sits in, found with floorDiv so that a
/// negative coordinate lands in the cell below it rather than the one above.
///
/// Formed in 64 bits and refused by name where a corner of the cell leaves
/// int32 (SPEC §8), which is within one cell of either end of an axis: a
/// pack reaches it on y through find_top_surface, whose scan starts at the
/// lattice point under its upper_bound, and a caller on any axis through
/// evaluate(). Everywhere else the corner is the int32 it always was. Out of
/// interpolate() so that its frame, which is on the stack once per level of
/// nested `interpolated`, holds the corner and not the check.
[[nodiscard]] Point cellCorner(const Point& at, const CellGeometry& cells) {
    const std::int64_t x0 = std::int64_t{javamath::floorDiv(at.x, cells.width)} * cells.width;
    const std::int64_t y0 = std::int64_t{javamath::floorDiv(at.y, cells.height)} * cells.height;
    const std::int64_t z0 = std::int64_t{javamath::floorDiv(at.z, cells.width)} * cells.width;
    if (!cellFits(x0, cells.width) || !cellFits(y0, cells.height) || !cellFits(z0, cells.width)) {
        refuseCell(at, cells, x0, y0, z0);
    }
    return Point{.x = static_cast<std::int32_t>(x0),
                 .y = static_cast<std::int32_t>(y0),
                 .z = static_cast<std::int32_t>(z0)};
}

} // namespace

Interpreter::ScopeArena::Lease Interpreter::ScopeArena::enter(const std::size_t nodeCount) {
    // See frames_: growing the vector of frames must move each frame, slot
    // buffer and all, and never copy one out from under an open scope.
    static_assert(std::is_nothrow_move_constructible_v<Frame>);
    if (depth_ == frames_.size()) {
        frames_.push_back(Frame{.slots = {}, .epoch = floor_});
    }
    Frame& frame = frames_[depth_];
    if (frame.slots.size() < nodeCount) {
        // Every stamp 0, which is at most any epoch: nothing reads as
        // computed, whatever the counter says.
        frame.slots.assign(nodeCount, Slot{});
    }
    ++frame.epoch;
    if (frame.epoch == 0) {
        // Wrapped. Every stamp in the frame is some earlier scope's epoch,
        // and the counter is about to hand those out again, so the frame is
        // forgotten first — exactly what zeroing it for every scope did,
        // once in 2^32 scopes instead of every time.
        for (Slot& slot : frame.slots) {
            slot.stamp = 0;
        }
        frame.epoch = 1;
        ++wraps_;
    }
    ++depth_;
    return Lease{.slots = frame.slots.data(), .epoch = frame.epoch};
}

void Interpreter::ScopeArena::advanceEpochs(const std::uint32_t epoch) noexcept {
    for (Frame& frame : frames_) {
        frame.epoch = std::max(frame.epoch, epoch);
    }
    floor_ = std::max(floor_, epoch);
}

/// One point's evaluation, with the values already computed for it. Nodes
/// are pure functions of the point, so this is only ever a speed-up — except
/// that without it a spline tree 47 knots wide over three shared coordinates
/// re-walks the same noise sampling thousands of times.
///
/// Its memo is a frame leased from @p arena for as long as the scope is open
/// (ScopeArena), given back when it closes — on the way out of a refusal
/// too, so a cache that saw one is as good as new for the next evaluation.
class Interpreter::Scope {
public:
    Scope(ScopeArena& arena, std::size_t nodeCount, Point at, CornerCache* cache,
          const FlatCacheWindow* window, ReadContext context)
        : arena_(&arena), frame_(arena.enter(nodeCount)), at_(at), cache_(cache), window_(window),
          context_(context) {}

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    ~Scope() { arena_->leave(); }

    /// Where every scope this one opens for another point leases its frame.
    [[nodiscard]] ScopeArena& arena() const noexcept { return *arena_; }

    [[nodiscard]] Point at() const noexcept { return at_; }

    /// Whose read this is: the generating block's, or a point it reads
    /// elsewhere. Carried into every scope a node opens for another point.
    [[nodiscard]] ReadContext context() const noexcept { return context_; }

    /// Where `interpolated` keeps its cell corners, or null when the caller
    /// supplied nowhere and every point pays for its own eight.
    [[nodiscard]] CornerCache* cache() const noexcept { return cache_; }

    /// Where `flat_cache` may relocate a read, or null for everywhere.
    [[nodiscard]] const FlatCacheWindow* window() const noexcept { return window_; }

    [[nodiscard]] bool has(NodeIndex index) const noexcept {
        return frame_.slots[static_cast<std::size_t>(index)].stamp == frame_.epoch;
    }

    [[nodiscard]] double get(NodeIndex index) const noexcept {
        return frame_.slots[static_cast<std::size_t>(index)].value;
    }

    // Not const, although it writes only through the lease's pointer: it is
    // what makes a node computed in this scope, and a const Scope (as
    // interpolate() is handed) must not be able to.
    // NOLINTNEXTLINE(readability-make-member-function-const)
    double store(NodeIndex index, double value) noexcept {
        ScopeArena::Slot& slot = frame_.slots[static_cast<std::size_t>(index)];
        slot.value = value;
        slot.stamp = frame_.epoch;
        return value;
    }

private:
    ScopeArena* arena_;
    ScopeArena::Lease frame_;
    Point at_;
    CornerCache* cache_ = nullptr;
    const FlatCacheWindow* window_ = nullptr;
    ReadContext context_ = ReadContext::Block;
};

std::optional<std::string_view> Interpreter::unevaluableReason(NodeType type) const noexcept {
    switch (type) {
        case NodeType::Interpolated:
        case NodeType::CacheAllInCell:
            if (cells_.has_value()) {
                return std::nullopt;
            }
            return "its value is defined by the cell it sits in, not by the point alone, so it "
                   "needs an interpreter built with a cell geometry — which comes from a noise "
                   "settings entry (SPEC §4.1)";
        case NodeType::Slide:
            return "it applies the vertical slides from a noise settings entry, which this "
                   "pipeline does not carry yet (SPEC §10, M3)";
        // WHAT NO LONGER HAS A CASE HERE, and why each left:
        //
        //   `find_top_surface` — settled (SPEC §11). It needs no cell
        //     lattice; the old refusal said it did and that was wrong. It
        //     scans its own column on a lattice of its own `cell_height`.
        //   `old_blended_noise` and `weird_scaled_sampler` — settled in M3,
        //     the first against vanilla's own values to within a part in a
        //     billion, the second from vanilla's changelog plus both rarity
        //     ladders measured off the server.
        //   `end_islands` — settled here. It was refused for the same reason
        //     the other three were, no documentation this project trusted and
        //     no oracle; the oracle turned out to be eight golden End regions
        //     already on disk, plus two probe regions for the parts of the
        //     field they cannot reach (noise::EndIslands' header, and
        //     tests/conformance/golden_end_test.cpp).
        //
        // All four reach the default deliberately. Each had a case of its own
        // saying so until clang-tidy pointed out that a branch identical to
        // the default is the default written twice.
        default:
            return std::nullopt;
    }
}

Interpreter::Interpreter(const Graph& graph, const NoiseRegistry& noises, CellGeometry cells)
    : Interpreter(graph, noises) {
    if (cells.width <= 0 || cells.height <= 0) {
        throw EvalError("a cell geometry of " + std::to_string(cells.width) + "x" +
                        std::to_string(cells.height) +
                        " is not usable: both dimensions must be positive");
    }
    cells_ = cells;
    // The lattice makes `interpolated` and `cache_all_in_cell` evaluable, so
    // the refusals the delegated constructor settled without one are
    // settled again with it.
    settleRefusals();
}

Interpreter::Interpreter(const Graph& graph, const NoiseRegistry& noises) : graph_(&graph) {
    ops_.assign(graph.nodeCount(), Op{});
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        const Node& node = graph.node(static_cast<NodeIndex>(i));
        Op& op = ops_[i];
        op.type = node.type;
        // The graph holds every node to its type's shape, and no type has
        // more operands than an Op does (kMaxNodeArguments,
        // kMaxNodeParameters, checked against the schema at compile time).
        // Refused rather than truncated all the same, should a graph ever
        // arrive by some other road.
        if (node.arguments.size() > op.arguments.size() ||
            node.parameters.size() > op.parameters.size()) {
            throw EvalError("'" + std::string(nodeTypeName(node.type)) + "' (node " +
                            std::to_string(i) + ") carries more operands than any type takes");
        }
        std::ranges::copy(node.arguments, op.arguments.begin());
        std::ranges::copy(node.parameters, op.parameters.begin());
        if (node.spline.has_value()) {
            op.hasSpline = true;
            op.spline = *node.spline;
        }
        if (node.type == NodeType::WeirdScaledSampler) {
            // The same two names rarityValueMapper() knows, and anything else
            // left for it to refuse at the sample.
            if (node.selector == "type_1") {
                op.rarity = Rarity::Type1;
            } else if (node.selector == "type_2") {
                op.rarity = Rarity::Type2;
            } else {
                op.rarity = Rarity::Unknown;
            }
        }
        if (!node.noise.has_value()) {
            // Either the node samples no noise, or it carries one inline.
            // The second is left unbound rather than refused here: a graph
            // is loaded once and sampled for one dimension at a time, and a
            // node nothing reaches should not stop the rest of it being
            // evaluated. refuseIfUnevaluable() is where it is refused.
            continue;
        }
        // A NAMED noise the registry does not hold is LEFT UNBOUND, NOT
        // THROWN — and the difference is a dimension. This used to throw
        // here. One graph carries all seven of vanilla's dimensions, so
        // construction-time refusal meant the registry had to satisfy every
        // OTHER dimension's noises before this one could be sampled at all —
        // and under a legacy source, where the registry is empty by
        // necessity (SPEC §11), that made the nether's `final_density`
        // unreachable because the OVERWORLD names `minecraft:cave_entrance`.
        // Nothing about that node is wrong; nothing reaches it.
        //
        // The refusal is not softened, only moved to where the reach is
        // known: refuseIfUnevaluable() throws at the node that actually
        // samples an unbuilt noise, naming it, and requireEvaluable(root)
        // still gives a caller the whole answer up front — ChunkFiller
        // ::compile calls it on final_density and on every router entry it
        // will sample, so a reached node is still refused before a single
        // block is filled. SPEC §8's rule is about not generating a wrong
        // world quietly, and a node no root reaches generates nothing.
        op.noise = noises.find(*node.noise);
    }

    // The blended noises, built once. Forty Perlin permutations each, from the
    // world seed and this node's own parameters — both fixed by the time a
    // pipeline is compiled, so building them per sample would be forty
    // thousand draws per block for no change in the answer.
    blendedOf_.assign(graph.nodeCount(), std::nullopt);
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        const Node& node = graph.node(static_cast<NodeIndex>(i));
        if (node.type != NodeType::OldBlendedNoise) {
            continue;
        }
        const noise::BlendedNoise::Parameters parameters{
            .xzScale = node.parameters[0],
            .yScale = node.parameters[1],
            .xzFactor = node.parameters[2],
            .yFactor = node.parameters[3],
            .smearScaleMultiplier = node.parameters[4],
        };
        // The dimension's declared random source picks the seeding, because
        // the two are genuinely different worlds and not a near miss: the
        // modern derivation scores at the empirical null in a legacy
        // dimension and vice versa (SPEC §11, measured on the server).
        //
        // THE LEGACY ARM IS REACHED. This comment used to say it was not,
        // and that stopped being true when `NoiseRegistry::create` was
        // narrowed to refuse only a Legacy source that is asked for a NAMED
        // noise: a legacy dimension's `final_density` names none, so it
        // builds an EMPTY registry and arrives here. It is the path the
        // legacy Nether's terrain measurement runs through —
        // tests/conformance/vanilla_legacy_nether_terrain_test.cpp, 99.99591%
        // of block classes over eight golden regions (six independent
        // worlds) — and the path the End generates its terrain on.
        blendedOf_[i] =
            noises.source() == RandomSource::Legacy
                ? noise::BlendedNoise::legacyFromWorldSeed(noises.worldSeed(), parameters)
                : noise::BlendedNoise::modern(noises.worldSeed(), parameters);
    }

    // The End island field, built once if anything asks for it. Unlike the
    // blended noises it takes no per-node parameters — every `end_islands`
    // node is the same function — so there is one per graph, not one per
    // node, and it is left empty when no node needs it rather than built
    // speculatively (it draws 256 LCG values).
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        if (graph.node(static_cast<NodeIndex>(i)).type == NodeType::EndIslands) {
            endIslands_ = noise::EndIslands::fromWorldSeed(noises.worldSeed());
            break;
        }
    }

    // Column invariance, in one forward pass. Node indices are assigned as
    // the resolver finishes each node, so a node's arguments — and the
    // coordinate of any spline it owns — always have smaller indices than it
    // does. That is the same property that makes the graph acyclic, and it
    // means nothing here has to be revisited.
    columnInvariant_.assign(graph.nodeCount(), 0);
    splineColumnInvariant_.assign(graph.splineCount(), 0);

    // Nested splines are pushed before the spline that owns them, so this
    // recursion always descends into something already settled or settleable.
    const auto settleSpline = [&graph, this](auto&& self, SplineIndex spline) -> bool {
        char& cached = splineColumnInvariant_[static_cast<std::size_t>(spline)];
        if (cached != 0) {
            return cached == 1;
        }
        const SplineDefinition& definition = graph.spline(spline);
        bool invariant = columnInvariant_[static_cast<std::size_t>(definition.coordinate)] != 0;
        for (const SplinePoint& point : definition.points) {
            if (point.nested.has_value() && !self(self, *point.nested)) {
                invariant = false;
            }
        }
        cached = invariant ? 1 : 2;
        return invariant;
    };

    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        const Node& node = graph.node(static_cast<NodeIndex>(i));
        const auto argumentsInvariant = [&node, this] {
            return std::ranges::all_of(node.arguments, [this](NodeIndex argument) {
                return columnInvariant_[static_cast<std::size_t>(argument)] != 0;
            });
        };

        // A noise written inline is refused rather than evaluated (see
        // refuseIfUnevaluable), and what will not be evaluated cannot be
        // shown to hold one value down a column either. Conservative, as
        // everything here is.
        if (node.inlineNoise.has_value()) {
            columnInvariant_[i] = 0;
            continue;
        }

        bool invariant = false;
        switch (node.type) {
            case NodeType::Constant:
            case NodeType::BlendAlpha:
            case NodeType::BlendOffset:
            case NodeType::ShiftA:
            case NodeType::ShiftB:
                // Constants and the two blend states do not vary at all;
                // shift_a and shift_b sample with y pinned, where plain
                // shift does not, which is the entire difference between
                // them.
                invariant = true;
                break;
            case NodeType::FlatCache:
                // flat_cache asks for whatever it wraps at y = 0 — inside
                // the window its grid covers. Outside one it reads its
                // argument where it was asked (SPEC §11), so it is only as
                // invariant as that argument.
                invariant = argumentsInvariant();
                break;
            case NodeType::Noise:
                invariant = isZero(node.parameters[1]);
                break;
            case NodeType::ShiftedNoise:
                invariant = isZero(node.parameters[1]) && argumentsInvariant();
                break;
            case NodeType::Spline:
                invariant = node.spline.has_value() && settleSpline(settleSpline, *node.spline);
                break;
            case NodeType::YClampedGradient:
            case NodeType::Shift:
            case NodeType::OldBlendedNoise:
            case NodeType::WeirdScaledSampler:
                // Each reads y itself, whatever its arguments do: the
                // gradient is a function of y, plain shift samples its noise
                // with y unpinned, old_blended_noise is 3D noise, and
                // weird_scaled_sampler samples its noise at the point's own
                // y over the rarity. The last two once reached the default
                // below, where a node with no varying argument —
                // old_blended_noise has none at all — was reported
                // invariant, and the End's biome source, trusting that, read
                // a datapack's 3D erosion once per chunk (measured in
                // tests/unit/biome_source_test.cpp).
                invariant = false;
                break;
            case NodeType::Interpolated:
                // Interpolating along y cannot remove a dependence on y, and
                // interpolating a column-invariant function cannot create
                // one: the four corner pairs it blends are equal.
                invariant = cells_.has_value() && argumentsInvariant();
                break;
            default:
                // What is left is a pure function of its arguments — or is
                // unevaluable, and then "varies" is the safe answer, though
                // the refusal comes first anyway.
                invariant = !unevaluableReason(node.type).has_value() && argumentsInvariant();
                break;
        }
        columnInvariant_[i] = invariant ? 1 : 0;
    }

    settleRefusals();
}

void Interpreter::settleRefusals() noexcept {
    for (std::size_t i = 0; i < ops_.size(); ++i) {
        ops_[i].refuses = refusalOf(static_cast<NodeIndex>(i)) != Refusal::None;
    }
}

void Interpreter::substitute(UnsettledSubstitutions substitutions) {
    substitutions_ = std::move(substitutions);
}

bool Interpreter::isColumnInvariant(NodeIndex index) const {
    if (index >= columnInvariant_.size()) {
        throw EvalError("node index " + std::to_string(index) + " is out of range");
    }
    return columnInvariant_[static_cast<std::size_t>(index)] != 0;
}

const noise::NormalNoise& Interpreter::noiseFor(NodeIndex index) const {
    const noise::NormalNoise* noise = ops_[static_cast<std::size_t>(index)].noise;
    if (noise == nullptr) {
        // The same sentence refuseIfUnevaluable() produces up front, so the
        // late refusal is as informative as the early one it replaced: the
        // node's TYPE and the noise's NAME, not a bare node index.
        const Node& node = graph_->node(index);
        if (node.noise.has_value()) {
            throw EvalError("'" + std::string(nodeTypeName(node.type)) + "' samples noise '" +
                            node.noise->toString() +
                            "', which was not built for this world seed. A dimension's "
                            "registry holds only the noises that dimension names");
        }
        throw EvalError("'" + std::string(nodeTypeName(node.type)) + "' (node " +
                        std::to_string(index) + ") samples a noise it does not name");
    }
    return *noise;
}

double Interpreter::sampleNoise(const NodeIndex index, const Point& at, const double x,
                                const double y, const double z) const {
    const noise::NormalNoise& noise = noiseFor(index);
    if (!noise.accepts(x, y, z)) {
        const Node& node = graph_->node(index);
        std::ostringstream message;
        message << std::setprecision(17) << "'" << nodeTypeName(node.type) << "' samples noise '"
                << (node.noise.has_value() ? node.noise->toString() : std::string("(inline)"))
                << "' at (" << x << ", " << y << ", " << z << ") for block (" << at.x << ", "
                << at.y << ", " << at.z << "), past " << noise.inputLimit()
                << " on an axis or not a number: there its highest-frequency octave's lattice "
                   "cell leaves the 32-bit range, where this build's arithmetic is undefined, "
                   "so it refuses rather than guess what the server builds there (SPEC §8)";
        throw EvalError(message.str());
    }
    return noise.sample(x, y, z);
}

const noise::BlendedNoise& Interpreter::blendedFor(NodeIndex index) const {
    const std::optional<noise::BlendedNoise>& blended = blendedOf_[static_cast<std::size_t>(index)];
    if (!blended.has_value()) {
        throw EvalError("node " + std::to_string(index) +
                        " is an old_blended_noise with no noise built for it");
    }
    return *blended;
}

namespace {

// The two ladders, written as ladders rather than as arithmetic because that
// is what they are: five and four steps with no pattern between them, and any
// formula that appeared to fit would be a coincidence over this many points.
// rarityValueMapper() picks one by name; an Op has picked it already.

[[nodiscard]] double rarityType1(const double input) noexcept {
    if (input < -0.5) {
        return 0.75;
    }
    if (input < 0.0) {
        return 1.0;
    }
    if (input < 0.5) {
        return 1.5;
    }
    return 2.0;
}

[[nodiscard]] double rarityType2(const double input) noexcept {
    if (input < -0.75) {
        return 0.5;
    }
    if (input < -0.5) {
        return 0.75;
    }
    if (input < 0.5) {
        return 1.0;
    }
    if (input < 0.75) {
        return 2.0;
    }
    return 3.0;
}

} // namespace

double rarityValueMapper(std::string_view mapper, double input) {
    if (mapper == "type_1") {
        return rarityType1(input);
    }
    if (mapper == "type_2") {
        return rarityType2(input);
    }
    throw EvalError("'minecraft:weird_scaled_sampler' has rarity_value_mapper '" +
                    std::string(mapper) +
                    "', which this build does not know; the two vanilla defines are 'type_1' "
                    "and 'type_2'");
}

Interpreter::Refusal Interpreter::refusalOf(const NodeIndex index) const noexcept {
    // Indexed unchecked: every caller holds an index the graph handed out, or
    // one refuseIfUnevaluable() has checked.
    const Node& node = graph_->nodes()[static_cast<std::size_t>(index)];
    if (unevaluableReason(node.type).has_value()) {
        return Refusal::Type;
    }
    if (node.noise.has_value() && ops_[static_cast<std::size_t>(index)].noise == nullptr) {
        return Refusal::UnbuiltNoise;
    }
    if (node.inlineNoise.has_value()) {
        return Refusal::InlineNoise;
    }
    return Refusal::None;
}

void Interpreter::refuseIfUnevaluable(const NodeIndex index) const {
    const Node& node = graph_->node(index);
    switch (refusalOf(index)) {
        case Refusal::None:
            return;
        case Refusal::Type:
            throw EvalError("'" + std::string(nodeTypeName(node.type)) +
                            "' cannot be evaluated by this build: " +
                            std::string(unevaluableReason(node.type).value_or("")));
        case Refusal::UnbuiltNoise:
            // Moved here from the constructor — see the note there. It used
            // to throw for every node in the graph, including the ones
            // belonging to other dimensions; it is refused here instead,
            // where "is this node reached" is known. Same loudness, same
            // name, and still before a single block is filled (SPEC §8).
            throw EvalError("'" + std::string(nodeTypeName(node.type)) + "' samples noise '" +
                            (node.noise.has_value() ? node.noise->toString() : std::string()) +
                            "', which was not built for this world seed. A dimension's registry "
                            "holds only the noises that dimension names");
        case Refusal::InlineNoise:
            // Not an admission that this build is behind: vanilla does not
            // build it either. The server takes such a pack, loads the JSON
            // without a word, and then dies constructing the dimension's
            // random state with NoSuchElementException from
            // Optional.orElseThrow, because the seed it needs is the MD5 of
            // the noise's identifier and this noise has none. Measured over
            // every field that takes the union, over two different sets of
            // parameters, and over a router entry the dimension never
            // samples: tools/analysis/inline-noise-probe.sh (SPEC §11).
            throw UnbuildableError(
                "'" + std::string(nodeTypeName(node.type)) +
                "' carries its noise parameters inline rather than naming a worldgen/noise "
                "entry. mcdoc allows that and this build loads it, but a noise is seeded from "
                "the MD5 of its identifier and this one has none — and the vanilla server does "
                "not work around that either: it refuses to build a world whose noise router "
                "reaches such a node. Name the parameters as a worldgen/noise entry (SPEC §11)");
    }
}

void Interpreter::requireEvaluable(NodeIndex root) const {
    std::vector<char> seen(graph_->nodeCount() + graph_->splineCount(), 0);
    requireEvaluableNode(root, seen);
}

void Interpreter::requireEvaluableNode(NodeIndex index, std::vector<char>& seen) const {
    if (seen[static_cast<std::size_t>(index)] != 0) {
        return;
    }
    seen[static_cast<std::size_t>(index)] = 1;

    refuseIfUnevaluable(index);
    const Node& node = graph_->node(index);

    // The subtree first, so that a refusal names the real cause. Column
    // invariance is deliberately conservative about a node it cannot
    // evaluate, and checking the cache before the contents made vanilla's
    // own `end/erosion` — a cache_2d over end_islands, which is as
    // column-invariant as a function gets — report that its argument varied
    // with y. That is a true refusal with a false reason attached, which is
    // worse than no reason at all.
    for (const NodeIndex argument : node.arguments) {
        requireEvaluableNode(argument, seen);
    }
    if (node.spline.has_value()) {
        requireEvaluableSpline(*node.spline, seen);
    }

    // cache_2d keys on (x, z) and never re-reads its argument as y changes.
    // Vanilla only ever wraps something column-invariant in it, so treating
    // it as transparent is exact — for vanilla. A pack that wrapped a
    // y-varying function would get a column of one value from vanilla and a
    // varying column from us, and that difference is far too quiet to ship.
    if (node.type == NodeType::Cache2d && !node.arguments.empty() &&
        !isColumnInvariant(node.arguments.front())) {
        throw EvalError(
            "'minecraft:cache_2d' wraps a function whose value varies with y. Vanilla caches it "
            "on (x, z) alone, so the whole column would take the value of whichever y was asked "
            "for first; this build refuses rather than pick one silently");
    }
}

void Interpreter::requireEvaluableSpline(SplineIndex index, std::vector<char>& seen) const {
    const std::size_t slot = graph_->nodeCount() + static_cast<std::size_t>(index);
    if (seen[slot] != 0) {
        return;
    }
    seen[slot] = 1;

    const SplineDefinition& spline = graph_->spline(index);
    requireEvaluableNode(spline.coordinate, seen);
    for (const SplinePoint& point : spline.points) {
        if (point.nested.has_value()) {
            requireEvaluableSpline(*point.nested, seen);
        }
    }
}

std::size_t Interpreter::cacheSize() const noexcept {
    return graph_->nodeCount();
}

void Interpreter::requireNode(const NodeIndex root) const {
    if (root >= graph_->nodeCount()) {
        throw EvalError("node index " + std::to_string(root) + " is out of range");
    }
}

// Each top-level call is where a ScopeArena is chosen: the caller's, through
// its CornerCache, which a chunk's whole fill shares; or one of this call's
// own when there is no cache, which is what a fresh Scope always was.

double Interpreter::evaluate(NodeIndex root, Point at, CornerCache& cache) const {
    requireNode(root);
    Scope scope(cache.scopes_, graph_->nodeCount(), at, &cache, nullptr, ReadContext::Block);
    return evaluateNode(scope, root);
}

double Interpreter::evaluate(NodeIndex root, Point at, CornerCache& cache,
                             const FlatCacheWindow& window, ReadContext context) const {
    requireNode(root);
    Scope scope(cache.scopes_, graph_->nodeCount(), at, &cache, &window, context);
    return evaluateNode(scope, root);
}

double Interpreter::evaluate(NodeIndex root, Point at) const {
    requireNode(root);
    ScopeArena arena;
    Scope scope(arena, graph_->nodeCount(), at, nullptr, nullptr, ReadContext::Block);
    return evaluateNode(scope, root);
}

double Interpreter::evaluateNode(Scope& scope, NodeIndex index) const {
    if (scope.has(index)) {
        return scope.get(index);
    }

    // Unchecked: a root is checked by evaluate(), and every index below it
    // is one the graph holds (Graph::nodes()).
    const Op& op = ops_[static_cast<std::size_t>(index)];
    if (op.refuses) {
        // Settled at construction (Op::refuses), so the refusal is the same,
        // with the same message, at the same visit, without being worked
        // out again on every visit to every node that draws none.
        refuseIfUnevaluable(index);
    }

    const Point at = scope.at();
    const auto x = static_cast<double>(at.x);
    const auto y = static_cast<double>(at.y);
    const auto z = static_cast<double>(at.z);

    const auto argument = [&](std::size_t which) {
        return evaluateNode(scope, op.arguments[which]);
    };

    double value = 0.0;
    switch (op.type) {
        case NodeType::Constant:
            value = op.parameters[0];
            break;

        // --- arithmetic ----------------------------------------------------
        case NodeType::Add: {
            // Sequenced through named locals, as every other multi-draw site
            // in this codebase is. The memo makes the order immaterial to
            // the answer here, but relying on that quietly is how the
            // NormalNoise stack swap happened on MSVC.
            const double first = argument(0);
            const double second = argument(1);
            value = first + second;
            break;
        }
        case NodeType::Mul: {
            const double first = argument(0);
            const double second = argument(1);
            value = first * second;
            break;
        }
        case NodeType::Min: {
            const double first = argument(0);
            const double second = argument(1);
            value = first < second ? first : second;
            break;
        }
        case NodeType::Max: {
            const double first = argument(0);
            const double second = argument(1);
            value = first > second ? first : second;
            break;
        }
        case NodeType::Abs:
            value = std::abs(argument(0));
            break;
        case NodeType::Square: {
            const double input = argument(0);
            value = input * input;
            break;
        }
        case NodeType::Cube: {
            const double input = argument(0);
            value = input * input * input;
            break;
        }
        case NodeType::HalfNegative: {
            const double input = argument(0);
            value = input < 0.0 ? input * 0.5 : input;
            break;
        }
        case NodeType::QuarterNegative: {
            const double input = argument(0);
            value = input < 0.0 ? input * 0.25 : input;
            break;
        }
        case NodeType::Squeeze: {
            const double input = std::clamp(argument(0), -1.0, 1.0);
            value = (input / 2.0) - ((input * input * input) / 24.0);
            break;
        }
        case NodeType::Invert:
            // Documented only through its later rename to `reciprocal`.
            //
            // Vanilla DOES use this — three times — and this comment used to
            // say otherwise. All three uses are inside
            // `preliminary_surface_level`, which find_top_surface refuses, so
            // no golden reaches it; that is a fact about what this build
            // cannot evaluate yet, not about what vanilla ships. It makes
            // this the only type vanilla uses that nothing checks (SPEC §11).
            value = 1.0 / argument(0);
            break;
        case NodeType::Clamp:
            value = std::clamp(argument(0), op.parameters[0], op.parameters[1]);
            break;
        case NodeType::RangeChoice: {
            const double input = argument(0);
            const bool inRange = input >= op.parameters[0] && input < op.parameters[1];
            // Only the chosen branch is evaluated: the other may be far more
            // expensive, and vanilla does not evaluate it either.
            value = inRange ? argument(1) : argument(2);
            break;
        }
        case NodeType::YClampedGradient:
            value = clampedMap(y, op.parameters[0], op.parameters[1], op.parameters[2],
                               op.parameters[3]);
            break;

        // --- noise ---------------------------------------------------------
        case NodeType::Noise: {
            const double xzScale = op.parameters[0];
            const double yScale = op.parameters[1];
            value = sampleNoise(index, at, x * xzScale, y * yScale, z * xzScale);
            break;
        }
        case NodeType::ShiftedNoise: {
            const double xzScale = op.parameters[0];
            const double yScale = op.parameters[1];
            const double shiftX = argument(0);
            const double shiftY = argument(1);
            const double shiftZ = argument(2);
            value = sampleNoise(index, at, (x * xzScale) + shiftX, (y * yScale) + shiftY,
                                (z * xzScale) + shiftZ);
            break;
        }
        case NodeType::Shift:
            value = sampleNoise(index, at, x * 0.25, y * 0.25, z * 0.25) * 4.0;
            break;
        case NodeType::ShiftA:
            value = sampleNoise(index, at, x * 0.25, 0.0, z * 0.25) * 4.0;
            break;
        case NodeType::ShiftB:
            // The axes are rotated, which is what makes shift_z independent
            // of shift_x while sharing one noise.
            value = sampleNoise(index, at, z * 0.25, x * 0.25, 0.0) * 4.0;
            break;

        // --- splines -------------------------------------------------------
        case NodeType::OldBlendedNoise:
            value = oldBlendedNoise(index, op, at);
            break;
        case NodeType::EndIslands:
            value = endIslands(at);
            break;
        case NodeType::FindTopSurface:
            value = findTopSurface(scope, op);
            break;
        case NodeType::WeirdScaledSampler:
            value = weirdScaledSampler(scope, index, op);
            break;
        case NodeType::Spline:
            // The resolver never builds one without a spline, but this layer
            // does not get to assume its input came from that resolver.
            if (!op.hasSpline) {
                throw EvalError("'minecraft:spline' carries no spline");
            }
            value = static_cast<double>(evaluateSpline(scope, op.spline));
            break;

        // --- caches --------------------------------------------------------
        case NodeType::FlatCache:
            value = flatCache(scope, op);
            break;
        case NodeType::BlendDensity:
            // The third member of the same interface as blend_alpha and
            // blend_offset, and this engine generates every chunk itself. If
            // 1.0 and 0.0 are what "not blending" means for those two — and
            // vanilla's own overworld/offset is a lerp that reduces to its
            // own value at alpha 1, which is hard to read any other way —
            // then not blending has to leave a density alone. Documented
            // nowhere, so SPEC §11 carries it until the goldens can say.
        case NodeType::Cache2d:
        case NodeType::CacheOnce:
        case NodeType::CacheAllInCell:
            // Memoisation that does not move the sample. Measured for all
            // three at the aquifer's reads, the one place a point is read off
            // its own block (SPEC §11, "Cache markers in a datapack's aquifer
            // entries"): cache_all_in_cell is the argument at the block and
            // at every detached point alike — not the cell's lower corner,
            // not a blend, not the generating block's value — and cache_once
            // and cache_2d are not one memo per block either: the ranked
            // sources each read their own. requireEvaluable has already
            // refused a cache_2d whose contents would make that untrue.
            value = argument(0);
            break;

        case NodeType::Interpolated:
            // Blended over the cell only for the block being generated (and
            // the cell corners it is blended from). A point that block reads
            // elsewhere — an aquifer source's centre, its contracted indices,
            // its surface anchors — reads the argument itself: measured on
            // every aquifer entry, where the blend is wrong on 61 295 to
            // 785 454 blocks an arm and seed, and the argument exact on every
            // block (SPEC §11).
            value = scope.context() == ReadContext::Detached ? argument(0)
                                                             : interpolate(op.arguments[0], scope);
            break;

        // --- blending ------------------------------------------------------
        case NodeType::BlendAlpha:
            // This engine generates every chunk itself and never blends
            // against terrain another generator wrote, which is the state
            // these two describe.
            value = 1.0;
            break;
        case NodeType::BlendOffset:
            value = 0.0;
            break;

        default:
            refuseUnevaluated(op.type);
    }

    return scope.store(index, value);
}

// --- the cases evaluateNode hands off ------------------------------------
//
// Each holds more than a few doubles, or builds a message, and evaluateNode's
// frame is paid once per level of a graph's depth (SPEC §8): out here, a
// case's locals are on the stack only while that case runs. Unoptimised,
// where a compiler gives every local and temporary a slot of its own, that
// took evaluateNode's frame from 3.1 KiB to 1.0 in the sanitize preset's
// build (SPEC §8).

double Interpreter::oldBlendedNoise(const NodeIndex index, const Op& op, const Point at) const {
    // Built at construction; see blendedOf_. A substitution still wins if one
    // is in place, so an experiment can put a candidate in front of the
    // settled reading.
    if (substitutions_.blendedNoise != nullptr) {
        const noise::BlendedNoise::Parameters parameters{.xzScale = op.parameters[0],
                                                         .yScale = op.parameters[1],
                                                         .xzFactor = op.parameters[2],
                                                         .yFactor = op.parameters[3],
                                                         .smearScaleMultiplier = op.parameters[4]};
        return substitutions_.blendedNoise(parameters, at);
    }
    return blendedFor(index).sample(at.x, at.y, at.z);
}

double Interpreter::endIslands(const Point at) const {
    // Built at construction, exactly once per graph; see endIslands_. Absent
    // only if the graph changed under the interpreter, which is a bug rather
    // than an input, so it is an error and not a fallback value.
    if (!endIslands_.has_value()) {
        throw EvalError("'minecraft:end_islands' was reached with no island field built for "
                        "this world seed");
    }
    return endIslands_->sample(at.x, at.z);
}

double Interpreter::findTopSurface(Scope& scope, const Op& op) const {
    // "Scans through a column of an input density and returns the topmost
    // y-level that is above 0. If no such position exists within the bounds,
    // the lower_bound is returned" — minecraft.wiki. Everything the wiki
    // leaves open was measured off the server (SPEC §11):
    //
    //   * the scan is on a lattice of ABSOLUTE multiples of cell_height,
    //     anchored to neither bound. Probed with an upper_bound of 317 and a
    //     lower_bound of -60, both off the lattice, and the answers stayed
    //     multiples of eight.
    //   * upper_bound is inclusive when it lands on the lattice, and is
    //     FLOORED onto it when it does not: 319.9 scans from 312, 320.0 scans
    //     from 320.
    //   * the test is a strict `> 0`: a density of exactly zero at a lattice
    //     point is not a surface.
    const Point at = scope.at();
    const double upperBound = evaluateNode(scope, op.arguments[1]);
    // Both conversions are defined: the graph refuses, at load, a lower_bound
    // outside the schema's [-4064, 4062] and a cell_height outside [1, 2^31)
    // (density_graph.cpp, checkNode; SPEC §8).
    const auto lowerBound = static_cast<std::int32_t>(op.parameters[0]);
    const auto cellHeight = static_cast<std::int32_t>(op.parameters[1]);
    if (cellHeight <= 0) {
        refuseCellHeight(cellHeight);
    }

    // upper_bound is a function's value, so it is checked here rather than at
    // load: a floor that is not an int32 (NaN included) has no defined
    // conversion, and nothing says what the server scans from there. Refused
    // by name (SPEC §8).
    const double flooredTop = std::floor(upperBound);
    if (std::isnan(flooredTop) || flooredTop < -2147483648.0 || flooredTop > 2147483647.0) {
        refuseUpperBound(upperBound, at);
    }
    const auto top = static_cast<std::int32_t>(flooredTop);
    // floorDiv, not truncating division: a negative upper_bound has to land
    // on the lattice point BELOW it, and C++ division rounds the other way.
    // The product is formed in 64 bits and refused where it leaves int32 — a
    // top within one cell_height of INT32_MIN — rather than wrapped.
    const std::int64_t startWide =
        static_cast<std::int64_t>(javamath::floorDiv(top, cellHeight)) * cellHeight;
    if (startWide < std::numeric_limits<std::int32_t>::min()) {
        refuseLatticePoint(upperBound, at, startWide);
    }
    const auto start = static_cast<std::int32_t>(startWide);

    // A bound on the work, so a pathological upper_bound is an error rather
    // than a hang. Vanilla's own use runs 48 steps.
    constexpr std::int64_t kMaxSteps = 1 << 20;
    const std::int64_t steps = (static_cast<std::int64_t>(start) - lowerBound) / cellHeight;
    if (steps > kMaxSteps) {
        refuseScan(steps, start, lowerBound);
    }

    // The step cannot overflow, which is a bound that holds rather than a
    // check: every scanY is a multiple of cell_height no lower than
    // lower_bound, and lower_bound is at least -4064. With a cell_height above
    // 4064 the only such multiples are 0 and up, so a step lands no lower than
    // -cell_height; with one at most 4064 it lands no lower than -8128.
    // Asserted, so that a change to the load checks that broke it would not
    // go unnoticed in a debug run.
    for (std::int32_t scanY = start; scanY >= lowerBound; scanY -= cellHeight) {
        assert(std::int64_t{scanY} - cellHeight >= std::numeric_limits<std::int32_t>::min());
        Scope column(scope.arena(), graph_->nodeCount(), Point{.x = at.x, .y = scanY, .z = at.z},
                     nullptr, scope.window(), scope.context());
        if (evaluateNode(column, op.arguments[0]) > 0.0) {
            return scanY;
        }
    }
    return lowerBound;
}

double Interpreter::weirdScaledSampler(Scope& scope, const NodeIndex index, const Op& op) const {
    // abs(rarity * noise(x/rarity, y/rarity, z/rarity)). The rarity scales the
    // sampled position AND the value, and the absolute value is what makes
    // this a cave function: the tunnels are where the result is near zero,
    // which is both sides of the noise's own zero crossing rather than one.
    //
    // A substituted constant still wins, as an isolation device.
    if (substitutions_.weirdScaledSampler.has_value()) {
        return *substitutions_.weirdScaledSampler;
    }
    const Point at = scope.at();
    const double input = evaluateNode(scope, op.arguments[0]);
    double rarity = 0.0;
    switch (op.rarity) {
        case Rarity::Type1:
            rarity = rarityType1(input);
            break;
        case Rarity::Type2:
            rarity = rarityType2(input);
            break;
        default:
            // A name the graph does not allow, refused by rarityValueMapper()
            // with the message it always gave, after the input as before.
            rarity = rarityValueMapper(graph_->node(index).selector, input);
            break;
    }
    return std::abs(rarity * sampleNoise(index, at, at.x / rarity, at.y / rarity, at.z / rarity));
}

double Interpreter::flatCache(Scope& scope, const Op& op) const {
    // Relocated, not merely remembered: vanilla fills this cache once per 4x4
    // column at y = 0, so every block in that column reads the corner's value
    // — inside the window that grid covers. Outside it there is no grid point
    // to read, and the argument is read where it was asked for, at its own
    // y: measured through every aquifer read a datapack can wrap in it, where
    // a y-varying argument makes that y visible (SPEC §11, "Cache markers in a
    // datapack's aquifer entries"). Without a window every read relocates.
    const Point at = scope.at();
    const FlatCacheWindow* window = scope.window();
    const bool relocates = window == nullptr || window->covers(at.x, at.z);
    const Point corner{.x = columnCorner(at.x), .y = 0, .z = columnCorner(at.z)};
    if (!relocates || corner == at) {
        return evaluateNode(scope, op.arguments[0]);
    }
    Scope cornerScope(scope.arena(), graph_->nodeCount(), corner, nullptr, window, scope.context());
    return evaluateNode(cornerScope, op.arguments[0]);
}

double Interpreter::interpolate(NodeIndex argument, const Scope& scope) const {
    const Point at = scope.at();
    // unevaluableReason refuses `interpolated` without a lattice, so this is
    // unreachable — but this layer does not get to assume its own caller
    // checked, and a missing lattice here would otherwise be a crash.
    if (!cells_.has_value()) {
        throw EvalError("'minecraft:interpolated' needs a cell geometry and this interpreter "
                        "has none");
    }
    const CellGeometry cells = *cells_;

    // The cell a point is in, refused by name where its corners leave int32
    // (cellCorner), so the corner arithmetic below stays inside it.
    const Point lower = cellCorner(at, cells);
    const std::int32_t x0 = lower.x;
    const std::int32_t y0 = lower.y;
    const std::int32_t z0 = lower.z;

    const double tx = static_cast<double>(at.x - x0) / static_cast<double>(cells.width);
    const double ty = static_cast<double>(at.y - y0) / static_cast<double>(cells.height);
    const double tz = static_cast<double>(at.z - z0) / static_cast<double>(cells.width);

    const auto corner = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
        // The corner's own scope carries the same cache: a nested
        // `interpolated` caches its own cell under its own node index, and
        // the cell check below keeps the two from being confused.
        Scope inner(scope.arena(), graph_->nodeCount(),
                    Point{.x = x0 + (dx * cells.width),
                          .y = y0 + (dy * cells.height),
                          .z = z0 + (dz * cells.width)},
                    scope.cache(), scope.window(), scope.context());
        return evaluateNode(inner, argument);
    };

    // With a cache, the eight corners are computed once for the cell and
    // reused; without one, every point computes its own. The two paths
    // must produce identical values, and the arithmetic below is shared
    // between them precisely so that they cannot drift apart.
    std::array<double, 8> own{};
    const std::array<double, 8>* corners = &own;
    CornerCache* cache = scope.cache();
    if (cache != nullptr) {
        CornerCache::Entry& entry = cache->entries_[static_cast<std::size_t>(argument)];
        if (!entry.filled || entry.x != x0 || entry.y != y0 || entry.z != z0) {
            entry.x = x0;
            entry.y = y0;
            entry.z = z0;
            entry.filled = true;
            entry.corners = {corner(0, 0, 0), corner(0, 0, 1), corner(0, 1, 0), corner(0, 1, 1),
                             corner(1, 0, 0), corner(1, 0, 1), corner(1, 1, 0), corner(1, 1, 1)};
        }
        corners = &entry.corners;
    } else {
        own = {corner(0, 0, 0), corner(0, 0, 1), corner(0, 1, 0), corner(0, 1, 1),
               corner(1, 0, 0), corner(1, 0, 1), corner(1, 1, 0), corner(1, 1, 1)};
    }

    // Sequenced through named locals, and blended y first, then x, then z.
    // Both matter: lerp is not associative across dimensions in floating
    // point, so a different order is a different world in the last bits.
    // The order is the documented one and is *not* yet checked against
    // vanilla — nothing available here samples terrain density — so SPEC §11
    // carries it as open until the goldens can settle it.
    const double v000 = (*corners)[0];
    const double v001 = (*corners)[1];
    const double v010 = (*corners)[2];
    const double v011 = (*corners)[3];
    const double v100 = (*corners)[4];
    const double v101 = (*corners)[5];
    const double v110 = (*corners)[6];
    const double v111 = (*corners)[7];

    const double x0z0 = lerp(ty, v000, v010);
    const double x1z0 = lerp(ty, v100, v110);
    const double x0z1 = lerp(ty, v001, v011);
    const double x1z1 = lerp(ty, v101, v111);

    const double nearZ = lerp(tx, x0z0, x1z0);
    const double farZ = lerp(tx, x0z1, x1z1);

    return lerp(tz, nearZ, farZ);
}

float Interpreter::evaluateSpline(Scope& scope, SplineIndex index) const {
    // Unchecked: a node's spline and a point's nested spline are indices the
    // graph holds (Graph::splines()).
    const SplineDefinition& spline = graph_->splines()[static_cast<std::size_t>(index)];
    const auto& points = spline.points;

    // Narrowed here, once: everything downstream is float, including the
    // comparison that picks the interval.
    const auto coordinate = static_cast<float>(evaluateNode(scope, spline.coordinate));

    const auto locationOf = [&points](std::size_t at) {
        return static_cast<float>(points[at].location);
    };
    const auto derivativeOf = [&points](std::size_t at) {
        return static_cast<float>(points[at].derivative);
    };
    const auto valueOf = [&](std::size_t at) {
        const SplinePoint& point = points[at];
        return point.nested.has_value() ? evaluateSpline(scope, *point.nested)
                                        : static_cast<float>(*point.value);
    };

    std::size_t upper = 0;
    while (upper < points.size() && locationOf(upper) < coordinate) {
        ++upper;
    }

    // Off either end, the spline continues as the straight line the nearest
    // knot's derivative describes, rather than flattening.
    if (upper == 0 || upper == points.size()) {
        const std::size_t nearest = upper == 0 ? 0 : points.size() - 1;
        return valueOf(nearest) + (derivativeOf(nearest) * (coordinate - locationOf(nearest)));
    }

    const std::size_t lower = upper - 1;
    const float lowerLocation = locationOf(lower);
    const float upperLocation = locationOf(upper);
    const float part = (coordinate - lowerLocation) / (upperLocation - lowerLocation);
    const float lowerValue = valueOf(lower);
    const float upperValue = valueOf(upper);
    const float span = upperLocation - lowerLocation;

    // The two ends of the cubic, expressed as how far each knot's tangent
    // overshoots the straight line between them.
    const float lowerTangent = (derivativeOf(lower) * span) - (upperValue - lowerValue);
    const float upperTangent = (-derivativeOf(upper) * span) + (upperValue - lowerValue);
    return lerp(part, lowerValue, upperValue) +
           (part * (1.0F - part) * lerp(part, lowerTangent, upperTangent));
}

} // namespace stratum::density
