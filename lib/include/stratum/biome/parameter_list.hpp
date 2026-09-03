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

#include <cstddef>
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

/// One axis of one entry. Vanilla writes either a `[min, max]` pair or a
/// single number, which is a range of zero width rather than a special case.
struct Parameter {
    double min = 0.0;
    double max = 0.0;

    /// How far @p value lies outside this range — zero when it is inside.
    /// This is the per-axis term of the distance the search minimises.
    [[nodiscard]] double distanceTo(double value) const noexcept {
        if (value < min) {
            return min - value;
        }
        if (value > max) {
            return value - max;
        }
        return 0.0;
    }

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

    /// Vanilla's fitness: the squared distance on each axis, summed, plus
    /// the offset squared. Squared throughout — no square root is taken,
    /// because only the ordering matters and taking one would add rounding
    /// to seven thousand comparisons per lookup.
    [[nodiscard]] double fitness(const ClimateSample& sample) const noexcept;

    [[nodiscard]] bool operator==(const ParameterPoint& other) const = default;
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

    /// The biome whose entry fits @p sample best. Ties go to the earlier
    /// entry, which is what makes the answer a function of the table's order
    /// as well as its contents — so the order is preserved as read.
    ///
    /// Throws ParameterError on an empty list rather than inventing a biome:
    /// there is no sensible default, and a world full of one wrong biome is
    /// worse than a refusal.
    [[nodiscard]] const data::ResourceLocation& find(const ClimateSample& sample) const;

private:
    std::vector<Entry> entries_;
};

} // namespace stratum::biome
