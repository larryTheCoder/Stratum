// Stratum — evaluating a resolved density function graph, one point at a time.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/noise/perlin.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::density {

namespace {

/// Zero, either sign of it. The project builds with -Wfloat-equal, and a
/// scale that is exactly zero is a structural fact about a node rather than
/// an approximate comparison.
[[nodiscard]] bool isZero(double value) noexcept {
    return (std::bit_cast<std::uint64_t>(value) & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0U;
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

} // namespace

/// One point's evaluation, with the values already computed for it. Nodes
/// are pure functions of the point, so this is only ever a speed-up — except
/// that without it a spline tree 47 knots wide over three shared coordinates
/// re-walks the same noise sampling thousands of times.
class Interpreter::Scope {
public:
    Scope(Point at, std::size_t nodeCount) : at_(at), values_(nodeCount), computed_(nodeCount, 0) {}

    [[nodiscard]] Point at() const noexcept { return at_; }

    [[nodiscard]] bool has(NodeIndex index) const noexcept {
        return computed_[static_cast<std::size_t>(index)] != 0;
    }

    [[nodiscard]] double get(NodeIndex index) const noexcept {
        return values_[static_cast<std::size_t>(index)];
    }

    double store(NodeIndex index, double value) noexcept {
        values_[static_cast<std::size_t>(index)] = value;
        computed_[static_cast<std::size_t>(index)] = 1;
        return value;
    }

private:
    Point at_;
    std::vector<double> values_;
    std::vector<char> computed_;
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
        case NodeType::FindTopSurface:
            // Settled (SPEC §11). It needs no cell lattice — the old refusal
            // said it did, and that was wrong; it scans its own column on a
            // lattice of its own `cell_height`.
            return std::nullopt;
        case NodeType::EndIslands:
            return "the End island field is not implemented yet; it arrives with the End's "
                   "terrain (SPEC §10, M3)";

        // `old_blended_noise` and `weird_scaled_sampler` were refused here
        // until M3 settled them (SPEC §11) — the first against vanilla's own
        // values to within a part in a billion, the second from vanilla's
        // changelog plus both rarity ladders measured off the server. They
        // reach the default deliberately, and had cases of their own until
        // clang-tidy pointed out that saying so twice is saying it once.
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
}

Interpreter::Interpreter(const Graph& graph, const NoiseRegistry& noises) : graph_(&graph) {
    noiseOf_.assign(graph.nodeCount(), nullptr);
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        const Node& node = graph.node(static_cast<NodeIndex>(i));
        if (!node.noise.has_value()) {
            // Either the node samples no noise, or it carries one inline.
            // The second is left unbound rather than refused here: a graph
            // is loaded once and sampled for one dimension at a time, and a
            // node nothing reaches should not stop the rest of it being
            // evaluated. refuseIfUnevaluable() is where it is refused.
            continue;
        }
        const noise::NormalNoise* noise = noises.find(*node.noise);
        if (noise == nullptr) {
            throw EvalError("'" + std::string(nodeTypeName(node.type)) + "' samples noise '" +
                            node.noise->toString() + "', which was not built for this world seed");
        }
        noiseOf_[i] = noise;
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
        blendedOf_[i] = noise::BlendedNoise::modern(noises.worldSeed(),
                                                    noise::BlendedNoise::Parameters{
                                                        .xzScale = node.parameters[0],
                                                        .yScale = node.parameters[1],
                                                        .xzFactor = node.parameters[2],
                                                        .yFactor = node.parameters[3],
                                                        .smearScaleMultiplier = node.parameters[4],
                                                    });
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
            case NodeType::FlatCache:
                // Constants and the two blend states do not vary at all;
                // shift_a and shift_b sample with y pinned, where plain
                // shift does not, which is the entire difference between
                // them; and flat_cache asks for whatever it wraps at y = 0.
                invariant = true;
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
    const noise::NormalNoise* noise = noiseOf_[static_cast<std::size_t>(index)];
    if (noise == nullptr) {
        throw EvalError("node " + std::to_string(index) + " samples a noise it does not name");
    }
    return *noise;
}

const noise::BlendedNoise& Interpreter::blendedFor(NodeIndex index) const {
    const std::optional<noise::BlendedNoise>& blended = blendedOf_[static_cast<std::size_t>(index)];
    if (!blended.has_value()) {
        throw EvalError("node " + std::to_string(index) +
                        " is an old_blended_noise with no noise built for it");
    }
    return *blended;
}

double rarityValueMapper(std::string_view mapper, double input) {
    // Written as ladders rather than as arithmetic because that is what they
    // are: five and four steps with no pattern between them, and any formula
    // that appeared to fit would be a coincidence over this many points.
    if (mapper == "type_1") {
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
    if (mapper == "type_2") {
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
    throw EvalError("'minecraft:weird_scaled_sampler' has rarity_value_mapper '" +
                    std::string(mapper) +
                    "', which this build does not know; the two vanilla defines are 'type_1' "
                    "and 'type_2'");
}

void Interpreter::refuseIfUnevaluable(const Node& node) const {
    if (const std::optional<std::string_view> reason = unevaluableReason(node.type)) {
        throw EvalError("'" + std::string(nodeTypeName(node.type)) +
                        "' cannot be evaluated by this build: " + std::string(*reason));
    }
    if (node.inlineNoise.has_value()) {
        // Not an admission that this build is behind: vanilla does not build
        // it either. The server takes such a pack, loads the JSON without a
        // word, and then dies constructing the dimension's random state with
        // NoSuchElementException from Optional.orElseThrow, because the seed
        // it needs is the MD5 of the noise's identifier and this noise has
        // none. Measured over every field that takes the union, over two
        // different sets of parameters, and over a router entry the dimension
        // never samples: tools/analysis/inline-noise-probe.sh (SPEC §11).
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

    const Node& node = graph_->node(index);
    refuseIfUnevaluable(node);

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

double Interpreter::evaluate(NodeIndex root, Point at) const {
    if (root >= graph_->nodeCount()) {
        throw EvalError("node index " + std::to_string(root) + " is out of range");
    }
    Scope scope(at, graph_->nodeCount());
    return evaluateNode(scope, root);
}

double Interpreter::evaluateNode(Scope& scope, NodeIndex index) const {
    if (scope.has(index)) {
        return scope.get(index);
    }

    const Node& node = graph_->node(index);
    refuseIfUnevaluable(node);

    const Point at = scope.at();
    const auto x = static_cast<double>(at.x);
    const auto y = static_cast<double>(at.y);
    const auto z = static_cast<double>(at.z);

    const auto argument = [&](std::size_t which) {
        return evaluateNode(scope, node.arguments[which]);
    };

    double value = 0.0;
    switch (node.type) {
        case NodeType::Constant:
            value = node.parameters[0];
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
            value = std::clamp(argument(0), node.parameters[0], node.parameters[1]);
            break;
        case NodeType::RangeChoice: {
            const double input = argument(0);
            const bool inRange = input >= node.parameters[0] && input < node.parameters[1];
            // Only the chosen branch is evaluated: the other may be far more
            // expensive, and vanilla does not evaluate it either.
            value = inRange ? argument(1) : argument(2);
            break;
        }
        case NodeType::YClampedGradient:
            value = clampedMap(y, node.parameters[0], node.parameters[1], node.parameters[2],
                               node.parameters[3]);
            break;

        // --- noise ---------------------------------------------------------
        case NodeType::Noise: {
            const double xzScale = node.parameters[0];
            const double yScale = node.parameters[1];
            value = noiseFor(index).sample(x * xzScale, y * yScale, z * xzScale);
            break;
        }
        case NodeType::ShiftedNoise: {
            const double xzScale = node.parameters[0];
            const double yScale = node.parameters[1];
            const double shiftX = argument(0);
            const double shiftY = argument(1);
            const double shiftZ = argument(2);
            value = noiseFor(index).sample((x * xzScale) + shiftX, (y * yScale) + shiftY,
                                           (z * xzScale) + shiftZ);
            break;
        }
        case NodeType::Shift:
            value = noiseFor(index).sample(x * 0.25, y * 0.25, z * 0.25) * 4.0;
            break;
        case NodeType::ShiftA:
            value = noiseFor(index).sample(x * 0.25, 0.0, z * 0.25) * 4.0;
            break;
        case NodeType::ShiftB:
            // The axes are rotated, which is what makes shift_z independent
            // of shift_x while sharing one noise.
            value = noiseFor(index).sample(z * 0.25, x * 0.25, 0.0) * 4.0;
            break;

        // --- splines -------------------------------------------------------
        case NodeType::OldBlendedNoise: {
            // Only reachable with a candidate in place; unevaluableReason
            // refuses it otherwise, and that refusal is what keeps an
            // unverified reading out of an ordinary world.
            // Built at construction; see blendedOf_. A substitution still
            // wins if one is in place, so an experiment can put a candidate
            // in front of the settled reading.
            if (substitutions_.blendedNoise != nullptr) {
                const noise::BlendedNoise::Parameters parameters{.xzScale = node.parameters[0],
                                                                 .yScale = node.parameters[1],
                                                                 .xzFactor = node.parameters[2],
                                                                 .yFactor = node.parameters[3],
                                                                 .smearScaleMultiplier =
                                                                     node.parameters[4]};
                value = substitutions_.blendedNoise(parameters, at);
            } else {
                value = blendedFor(index).sample(at.x, at.y, at.z);
            }
            break;
        }
        case NodeType::FindTopSurface: {
            // "Scans through a column of an input density and returns the
            // topmost y-level that is above 0. If no such position exists
            // within the bounds, the lower_bound is returned" — minecraft.wiki.
            // Everything the wiki leaves open was measured off the server
            // (SPEC §11):
            //
            //   * the scan is on a lattice of ABSOLUTE multiples of
            //     cell_height, anchored to neither bound. Probed with an
            //     upper_bound of 317 and a lower_bound of -60, both off the
            //     lattice, and the answers stayed multiples of eight.
            //   * upper_bound is inclusive when it lands on the lattice, and
            //     is FLOORED onto it when it does not: 319.9 scans from 312,
            //     320.0 scans from 320.
            //   * the test is a strict `> 0`: a density of exactly zero at a
            //     lattice point is not a surface.
            const double upperBound = evaluateNode(scope, node.arguments[1]);
            const auto lowerBound = static_cast<std::int32_t>(node.parameters[0]);
            const auto cellHeight = static_cast<std::int32_t>(node.parameters[1]);
            if (cellHeight <= 0) {
                throw EvalError("'minecraft:find_top_surface' has cell_height " +
                                std::to_string(cellHeight) +
                                ", which would never finish scanning; it must be positive");
            }

            // floorDiv, not truncating division: a negative upper_bound has to
            // land on the lattice point BELOW it, and C++ division rounds the
            // other way.
            const auto top = static_cast<std::int32_t>(std::floor(upperBound));
            const std::int32_t start = javamath::floorDiv(top, cellHeight) * cellHeight;

            // A bound on the work, so a pathological upper_bound is an error
            // rather than a hang. Vanilla's own use runs 48 steps.
            constexpr std::int64_t kMaxSteps = 1 << 20;
            const std::int64_t steps = (static_cast<std::int64_t>(start) - lowerBound) / cellHeight;
            if (steps > kMaxSteps) {
                throw EvalError("'minecraft:find_top_surface' would scan " + std::to_string(steps) +
                                " steps from " + std::to_string(start) + " down to " +
                                std::to_string(lowerBound) +
                                "; that is not a column, it is a hang");
            }

            value = lowerBound;
            for (std::int32_t scanY = start; scanY >= lowerBound; scanY -= cellHeight) {
                Scope column(Point{.x = at.x, .y = scanY, .z = at.z}, graph_->nodeCount());
                if (evaluateNode(column, node.arguments[0]) > 0.0) {
                    value = scanY;
                    break;
                }
            }
            break;
        }
        case NodeType::WeirdScaledSampler: {
            // abs(rarity * noise(x/rarity, y/rarity, z/rarity)). The rarity
            // scales the sampled position AND the value, and the absolute
            // value is what makes this a cave function: the tunnels are where
            // the result is near zero, which is both sides of the noise's
            // own zero crossing rather than one.
            //
            // A substituted constant still wins, as an isolation device.
            if (substitutions_.weirdScaledSampler.has_value()) {
                value = *substitutions_.weirdScaledSampler;
                break;
            }
            const double rarity =
                rarityValueMapper(node.selector, evaluateNode(scope, node.arguments[0]));
            value = std::abs(rarity *
                             noiseFor(index).sample(at.x / rarity, at.y / rarity, at.z / rarity));
            break;
        }
        case NodeType::Spline: {
            // The resolver never builds one without a spline, but this layer
            // does not get to assume its input came from that resolver.
            if (!node.spline.has_value()) {
                throw EvalError("'minecraft:spline' carries no spline");
            }
            value = static_cast<double>(evaluateSpline(scope, *node.spline));
            break;
        }

        // --- caches --------------------------------------------------------
        case NodeType::FlatCache: {
            // Relocated, not merely remembered: vanilla fills this cache
            // once per 4x4 column at y = 0, so every block in that column
            // reads the corner's value.
            const Point corner{.x = columnCorner(at.x), .y = 0, .z = columnCorner(at.z)};
            if (corner == at) {
                value = argument(0);
            } else {
                Scope cornerScope(corner, graph_->nodeCount());
                value = evaluateNode(cornerScope, node.arguments[0]);
            }
            break;
        }
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
            // Memoisation that does not move the sample. cache_all_in_cell
            // keeps one value per block of a cell rather than one per cell,
            // so like the other two it is a cache and not a relocation.
            // requireEvaluable has already refused a cache_2d whose contents
            // would make that untrue.
            value = argument(0);
            break;

        case NodeType::Interpolated:
            value = interpolate(node.arguments[0], at);
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
            throw EvalError("'" + std::string(nodeTypeName(node.type)) +
                            "' has no evaluation in this build");
    }

    return scope.store(index, value);
}

double Interpreter::interpolate(NodeIndex argument, Point at) const {
    // unevaluableReason refuses `interpolated` without a lattice, so this is
    // unreachable — but this layer does not get to assume its own caller
    // checked, and a missing lattice here would otherwise be a crash.
    if (!cells_.has_value()) {
        throw EvalError("'minecraft:interpolated' needs a cell geometry and this interpreter "
                        "has none");
    }
    const CellGeometry cells = *cells_;

    // The cell a point is in, found with floorDiv so that a negative
    // coordinate lands in the cell below it rather than the one above.
    const std::int32_t x0 = javamath::floorDiv(at.x, cells.width) * cells.width;
    const std::int32_t y0 = javamath::floorDiv(at.y, cells.height) * cells.height;
    const std::int32_t z0 = javamath::floorDiv(at.z, cells.width) * cells.width;

    const double tx = static_cast<double>(at.x - x0) / static_cast<double>(cells.width);
    const double ty = static_cast<double>(at.y - y0) / static_cast<double>(cells.height);
    const double tz = static_cast<double>(at.z - z0) / static_cast<double>(cells.width);

    const auto corner = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
        Scope scope(Point{.x = x0 + (dx * cells.width),
                          .y = y0 + (dy * cells.height),
                          .z = z0 + (dz * cells.width)},
                    graph_->nodeCount());
        return evaluateNode(scope, argument);
    };

    // Sequenced through named locals, and blended y first, then x, then z.
    // Both matter: lerp is not associative across dimensions in floating
    // point, so a different order is a different world in the last bits.
    // The order is the documented one and is *not* yet checked against
    // vanilla — nothing available here samples terrain density — so SPEC §11
    // carries it as open until the goldens can settle it.
    const double v000 = corner(0, 0, 0);
    const double v001 = corner(0, 0, 1);
    const double v010 = corner(0, 1, 0);
    const double v011 = corner(0, 1, 1);
    const double v100 = corner(1, 0, 0);
    const double v101 = corner(1, 0, 1);
    const double v110 = corner(1, 1, 0);
    const double v111 = corner(1, 1, 1);

    const double x0z0 = lerp(ty, v000, v010);
    const double x1z0 = lerp(ty, v100, v110);
    const double x0z1 = lerp(ty, v001, v011);
    const double x1z1 = lerp(ty, v101, v111);

    const double nearZ = lerp(tx, x0z0, x1z0);
    const double farZ = lerp(tx, x0z1, x1z1);

    return lerp(tz, nearZ, farZ);
}

float Interpreter::evaluateSpline(Scope& scope, SplineIndex index) const {
    const SplineDefinition& spline = graph_->spline(index);
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
