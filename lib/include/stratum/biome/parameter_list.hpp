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
/// reason this is not simply `llround(value * 10000)`.
[[nodiscard]] constexpr std::int64_t quantizeCoord(double value) noexcept {
    return static_cast<std::int64_t>(static_cast<float>(value) * 10000.0F);
}

/// One axis of one entry. Vanilla writes either a `[min, max]` pair or a
/// single number, which is a range of zero width rather than a special case.
struct Parameter {
    double min = 0.0;
    double max = 0.0;

    [[nodiscard]] bool operator==(const Parameter& other) const = default;
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
    /// The widest legal input is a bound of ±2 against a sample of ∓2, so
    /// each term is at most 40000² and the sum of seven at most 1.12e10 —
    /// comfortably inside int64, and no term can overflow.
    [[nodiscard]] std::int64_t fitness(const QuantizedSample& sample) const noexcept;

    [[nodiscard]] bool operator==(const QuantizedPoint& other) const = default;
};

struct Entry {
    ParameterPoint parameters;
    data::ResourceLocation biome{"minecraft", "plains"};
};

/// A dimension's table, and the search over it.
class ParameterList {
public:
    /// Reads the shape the server's data generator writes. @p id names the
    /// list in any error raised.
    [[nodiscard]] static ParameterList fromJson(const nlohmann::json& json,
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
    /// Throws ParameterError on an empty list rather than inventing a biome:
    /// there is no sensible default, and a world full of one wrong biome is
    /// worse than a refusal.
    [[nodiscard]] const data::ResourceLocation& find(const ClimateSample& sample) const;

private:
    std::vector<Entry> entries_;
    /// entries_[i] in the space the search runs in. Kept parallel rather
    /// than inside Entry so that Entry stays the table as it was written.
    std::vector<QuantizedPoint> quantized_;
};

} // namespace stratum::biome
