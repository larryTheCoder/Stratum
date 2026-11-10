// Stratum — a configured carver, as a pack writes it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `worldgen/configured_carver/*.json` and the inline definitions a biome's
// `carvers` field may hold (spec/carvers-b-spec.md Q11.1). The three types
// vanilla uses, `minecraft:cave`, `minecraft:nether_cave` and
// `minecraft:canyon`, and their fields (spec/carvers-a-spec.md Q13.1-Q13.4,
// spec/carvers-b-spec.md Q13.1). Anything else a pack names — another carver
// type, a float provider other than constant, uniform or trapezoid, a height
// provider other than uniform or constant, a field these types do not have —
// is refused by name, never read as something near it (SPEC §8).
//
// `debug_settings` is read in full (spec/carvers-a-spec.md Q13.4 as amended:
// parity applies, so a pack that sets `debug_mode` gets the debug output the
// server writes; confirmed on tools/analysis/carver-golden-probe.sh
// --variant debug, SPEC §11).

#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace stratum::carver {

class CarverError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class CarverType : std::uint8_t { Cave, NetherCave, Canyon };

/// A float provider (spec A-Q1.5, B-Q1.3). Values are the pack's floats.
struct FloatProvider {
    enum class Kind : std::uint8_t { Constant, Uniform, Trapezoid };
    Kind kind = Kind::Constant;
    /// Constant: the value. Uniform: `min_inclusive`, `max_exclusive`.
    /// Trapezoid: `min`, `max`, `plateau`.
    float a = 0.0F;
    float b = 0.0F;
    float c = 0.0F;

    /// Whether sampling it draws from the generator.
    [[nodiscard]] bool draws() const noexcept { return kind != Kind::Constant; }
};

/// A height provider: a uniform over two anchors, or a constant anchor
/// (spec A-Q1.5, A-Q13.3).
struct HeightProvider {
    enum class Kind : std::uint8_t { Constant, Uniform };
    Kind kind = Kind::Uniform;
    surface::VerticalAnchor minInclusive;
    surface::VerticalAnchor maxInclusive;

    [[nodiscard]] bool operator==(const HeightProvider&) const = default;
};

/// `replaceable`: a tag, or block ids written out (spec A-Q12.2).
struct Replaceable {
    /// Set when the field names a tag (`#namespace:path`).
    bool isTag = false;
    data::ResourceLocation tag{"minecraft", "air"};
    std::vector<data::ResourceLocation> blocks;

    [[nodiscard]] bool operator==(const Replaceable&) const = default;
};

/// The canyon's `shape` (spec B-Q10.2, B-Q13.1).
struct CanyonShape {
    FloatProvider distanceFactor;
    FloatProvider thickness;
    std::int32_t widthSmoothness = 1;
    FloatProvider horizontalRadiusFactor;
    float verticalRadiusDefaultFactor = 1.0F;
    float verticalRadiusCenterFactor = 0.0F;
};

/// `debug_settings` (spec A-Q13.4). The states are the pack's, as written.
struct DebugSettings {
    bool debugMode = false;
    settings::BlockState airState;
    settings::BlockState waterState;
    settings::BlockState lavaState;
    settings::BlockState barrierState;

    /// What an omitted `debug_settings` holds: debug off, and the four
    /// default states (which nothing then reads).
    [[nodiscard]] static DebugSettings omitted();

    [[nodiscard]] bool operator==(const DebugSettings&) const = default;
};

struct ConfiguredCarver {
    CarverType type = CarverType::Cave;
    float probability = 0.0F;
    HeightProvider y;
    FloatProvider yScale;
    surface::VerticalAnchor lavaLevel;
    Replaceable replaceable;
    DebugSettings debug = DebugSettings::omitted();

    // cave and nether_cave (spec A-Q13.2).
    FloatProvider horizontalRadiusMultiplier;
    FloatProvider verticalRadiusMultiplier;
    FloatProvider floorLevel;

    // canyon (spec B-Q13.1).
    FloatProvider verticalRotation;
    CanyonShape shape;

    /// Parses a configured carver file's JSON (`{"type": ..., "config":
    /// {...}}`). @p id names it in every refusal.
    [[nodiscard]] static ConfiguredCarver fromJson(const nlohmann::json& json,
                                                   const std::string& id);
};

/// One entry of a biome's `carvers` list: a reference to a configured
/// carver, or a definition written inline (spec B-Q11.1).
using CarverEntry = std::variant<data::ResourceLocation, ConfiguredCarver>;

/// A biome's `carvers` field, in each of the forms vanilla's own data uses
/// (spec B-Q11.1): a bare id string (a one-element list), a list of ids
/// and inline definitions, or — what 1.21.11's data writes for "none" —
/// an empty list. A list position is the entry's `k` (spec A-Q11.3).
/// @p biome names it in every refusal.
[[nodiscard]] std::vector<CarverEntry> parseBiomeCarvers(const nlohmann::json& field,
                                                         const std::string& biome);

} // namespace stratum::carver
