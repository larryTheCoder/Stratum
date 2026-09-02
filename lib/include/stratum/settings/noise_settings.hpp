// Stratum — noise settings: a dimension's geometry, flags and noise router.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A `worldgen/noise_settings` entry is what turns a pile of density
// functions into a dimension: how tall it is, how big a cell is, what block
// fills it, and which fifteen functions the generator asks for. Everything
// M3 does — cell sampling, interpolation, the aquifer decision — is
// parameterised by this file, which is why it comes first.
//
// THE FIELD NAMES ARE NOT WRITTEN DOWN HERE. They are generated from mcdoc
// by tools/mcdoc-sync, and that is not ceremony: at 1.21.9 the router's
// `initial_density_without_jaggedness` became `preliminary_surface_level`.
// Anyone writing the list from memory would have written the older name,
// every vanilla noise settings file would have been refused, and the reason
// would not have been obvious from the error. The schema knows; memory does
// not.
//
// WHAT IS LOADED BUT NOT INTERPRETED. `surface_rule` and `spawn_target` are
// M4. They are kept as parsed JSON rather than dropped, so that a caller can
// see they exist and a stored pipeline (SPEC §6) can round-trip them; no
// part of this build reads them, and validate says so out loud.

#pragma once

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace stratum::settings {

/// Raised when a noise settings entry cannot be read. The message names the
/// entry and the field, because a pack with seven of them needs to be told
/// which one is wrong.
class SettingsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A block state as noise settings writes one: a name, and the properties
/// that pin down which of its states is meant.
struct BlockState {
    data::ResourceLocation name{"minecraft", "air"};
    /// Kept as strings. This layer has no block registry to check them
    /// against — that arrives with the Bedrock mapping (SPEC §9) — and
    /// inventing one here would mean two sources of truth about block names.
    std::map<std::string, std::string> properties;

    [[nodiscard]] bool operator==(const BlockState& other) const = default;
};

/// The vertical extent of a dimension and the shape of one noise cell.
struct NoiseGeometry {
    std::int32_t minY = 0;
    std::int32_t height = 0;
    /// Cell size in units of four blocks, which is the only shape vanilla's
    /// schema allows (1..4, so 4 to 16 blocks).
    std::int32_t sizeHorizontal = 0;
    std::int32_t sizeVertical = 0;

    [[nodiscard]] std::int32_t cellWidth() const noexcept { return sizeHorizontal * 4; }

    [[nodiscard]] std::int32_t cellHeight() const noexcept { return sizeVertical * 4; }

    /// One past the top: a dimension covers [minY, maxY).
    [[nodiscard]] std::int32_t maxY() const noexcept { return minY + height; }

    /// How many cells tall the dimension is. Exact, because the loader
    /// refuses a height that is not a whole number of cells.
    [[nodiscard]] std::int32_t cellCountY() const noexcept { return height / cellHeight(); }

    [[nodiscard]] bool operator==(const NoiseGeometry& other) const = default;
};

/// Which entry of the noise router. Generated from mcdoc, so that a rename
/// between versions is a compile error at every use site rather than a
/// lookup that quietly finds nothing.
enum class RouterEntry : std::uint8_t {
#include <stratum/settings/router_entries.inc>
    Count,
};

inline constexpr std::size_t kRouterEntryCount = static_cast<std::size_t>(RouterEntry::Count);

/// "preliminary_surface_level".
[[nodiscard]] std::string_view routerEntryName(RouterEntry entry) noexcept;

/// The fifteen density functions a dimension's generator asks for, resolved
/// into the same graph as the pack's named ones.
struct NoiseRouter {
    std::array<density::NodeIndex, kRouterEntryCount> entries{};

    [[nodiscard]] density::NodeIndex at(RouterEntry entry) const {
        return entries[static_cast<std::size_t>(entry)];
    }
};

struct NoiseSettings {
    data::ResourceLocation id{"minecraft", "overworld"};
    BlockState defaultBlock;
    BlockState defaultFluid;
    std::int32_t seaLevel = 0;
    bool disableMobGeneration = false;
    bool aquifersEnabled = false;
    bool oreVeinsEnabled = false;
    bool legacyRandomSource = false;
    NoiseGeometry geometry;
    NoiseRouter router;

    /// Loaded, kept, not interpreted (M4). See the file comment.
    nlohmann::json surfaceRule;
    nlohmann::json spawnTarget;

    /// Reads one entry, resolving its router through @p builder so that its
    /// functions share a graph with the pack's named ones.
    [[nodiscard]] static NoiseSettings load(const data::PackEntry& entry,
                                            density::Graph::Builder& builder);
};

/// Reads every `worldgen/noise_settings` entry in @p pack and the density
/// functions it names, into one graph. This is the whole 3D pipeline's load
/// step: what comes back is everything M3 needs to sample a cell, short of a
/// seed.
struct LoadedSettings {
    density::Graph graph;
    std::map<data::ResourceLocation, NoiseSettings> settings;
};

[[nodiscard]] LoadedSettings loadAll(const data::Pack& pack);

} // namespace stratum::settings
