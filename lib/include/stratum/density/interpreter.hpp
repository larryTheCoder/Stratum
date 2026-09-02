// Stratum — evaluating a resolved density function graph, one point at a time.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §4.1 permits exactly this as a Milestone-M2 stepping stone: "the
// resolved graph is compiled into a flat execution program ... interpretation
// of the graph per block is acceptable only as a Milestone-M2 stepping
// stone." The compiled program arrives in M3; this is what makes the 2D
// values comparable against an oracle before then.
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
// Everything else is evaluated. The 2D chain vanilla's overworld actually
// uses — shift_a/shift_b, flat_cache, cache_2d, shifted_noise, noise, spline
// and the arithmetic around them — is checked bit-exactly against cubiomes
// in tests/conformance/vanilla_climate_test.cpp.
//
// CACHE NODES. CLAUDE.md calls these parity-critical, and they are, so it is
// worth being exact about what this layer does with them. `flat_cache`
// relocates its sample to the corner of the 4x4 column it sits in, at y=0,
// and that changes the value at a point, so it is implemented. `cache_2d`
// and `cache_once` are memoisation that does not move the sample, so at a
// single point they are transparent — and `cache_2d` is transparent only
// because what vanilla wraps in it never varies with y, which this layer
// checks rather than assumes. `interpolated` and `cache_all_in_cell` do move
// the sample, but to somewhere only the cell structure defines, so they are
// refused rather than approximated.

#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/noise/perlin.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace stratum::density {

/// Raised when a graph cannot be evaluated: a node type this build does not
/// implement, a missing noise, a cache whose contents break its contract.
class EvalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
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

class Interpreter {
public:
    /// Binds a graph to the noises it names. Throws EvalError if any `noise`
    /// field has nothing behind it, so that a missing noise is found once at
    /// construction rather than on whichever chunk first reached that node.
    Interpreter(const Graph& graph, const NoiseRegistry& noises);

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

    /// The value of @p root at @p at.
    [[nodiscard]] double evaluate(NodeIndex root, Point at) const;

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
    [[nodiscard]] bool isColumnInvariant(NodeIndex index) const;

private:
    /// One point's evaluation. Every node in a graph is a pure function of
    /// the point, so a node reached twice — `shift_x` is referenced five
    /// times by vanilla's overworld — is computed once and remembered. This
    /// is not the compiled program SPEC §4.1 asks for; it is what keeps the
    /// interpreter from being exponential in the depth of a spline tree.
    class Scope;

    [[nodiscard]] double evaluateNode(Scope& scope, NodeIndex index) const;

    /// Splines are evaluated in float, not double: their knot locations,
    /// derivatives and values are floats in vanilla, and the coordinate is
    /// narrowed to float before it is used. Widening the arithmetic would
    /// change the last bits of every terrain offset in the world.
    [[nodiscard]] float evaluateSpline(Scope& scope, SplineIndex index) const;

    [[nodiscard]] const noise::NormalNoise& noiseFor(NodeIndex index) const;

    void requireEvaluableNode(NodeIndex index, std::vector<char>& seen) const;
    void requireEvaluableSpline(SplineIndex index, std::vector<char>& seen) const;

    /// Trilinear interpolation over the eight corners of @p at's cell.
    /// Vanilla computes each corner once per cell and reuses it across every
    /// block in that cell; this recomputes them per point, which is the same
    /// value at eight times the cost — the density function is a pure
    /// function of position. The chunk filler is what makes that once-per-cell
    /// again (SPEC §4.1).
    [[nodiscard]] double interpolate(NodeIndex argument, Point at) const;

    const Graph* graph_;
    std::optional<CellGeometry> cells_;
    /// The noise each node samples, resolved once, indexed by node. Null for
    /// the nodes that sample none.
    std::vector<const noise::NormalNoise*> noiseOf_;
    /// Column invariance per node, computed once at construction rather than
    /// memoised on demand: an interpreter is shared across threads and const
    /// has to mean const (SPEC §4.1).
    std::vector<char> columnInvariant_;
    std::vector<char> splineColumnInvariant_;
};

} // namespace stratum::density
