// Stratum — the carvers: cave, nether_cave and canyon, carving one chunk.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// spec/carvers-a-spec.md (cave, nether_cave, the shared machinery) and
// spec/carvers-b-spec.md (canyon), claim by claim; each formula below names
// the claim it implements, and evaluates in the claim's `fp-form` order:
// float where the claim says single precision (every intermediate rounded to
// float, which -ffp-contract=off and SSE/NEON scalar arithmetic guarantee),
// double where it says double.
//
// A target chunk is carved by re-running every start in its 17x17 window of
// start chunks, `Sx - Tx` outer, `Sz - Tz` inner (A-Q8.1, A-Q11.1), each
// start chunk's carvers in its biome's list order (A-Q11.2, A-Q11.3). Every
// walk is a pure function of the world seed, the start and its carver
// (A-Q2.2); only the cells inside the target are decided (A-Q4), through
// one mask (A-Q4.4), in x, z ascending and y descending within an
// ellipsoid (A-Q4.5).
//
// Generation-time state is per call (SPEC §4.1): the compiled carvers are
// immutable and shared; the mask, the aquifer-flag history and the grass
// flags live in carveChunk's frame.

#pragma once

#include <stratum/carver/config.hpp>
#include <stratum/data/block_tags.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace stratum::carver {

/// A configured carver, with its `replaceable` resolved to block ids.
struct CompiledCarver {
    ConfiguredCarver config;
    std::set<data::ResourceLocation> replaceable;

    /// A debug state as the carver writes it, and whether it holds fluid
    /// (which is what a mark asks, A-Q12.4).
    struct DebugWrite {
        settings::BlockState state;
        bool holdsFluid = false;
    };

    /// `debug_settings`' four states, prepared (A-Q13.4): the water state
    /// with its `waterlogged` property, where it has one, set true. Read
    /// only when `config.debug.debugMode` is set.
    DebugWrite debugAir;
    DebugWrite debugWater;
    DebugWrite debugLava;
    DebugWrite debugBarrier;
};

/// Every biome's carver list, compiled (A-Q11.2): immutable once built.
class CarverSet {
public:
    void setBiome(const data::ResourceLocation& biome,
                  std::vector<std::shared_ptr<const CompiledCarver>> carvers);

    /// The biome's list, in order; empty for a biome that carves nothing or
    /// that the set does not hold.
    [[nodiscard]] const std::vector<std::shared_ptr<const CompiledCarver>>&
    carversOf(const data::ResourceLocation& biome) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return biomes_.empty(); }

    /// Every biome's list, by biome.
    [[nodiscard]] const std::map<data::ResourceLocation,
                                 std::vector<std::shared_ptr<const CompiledCarver>>>&
    biomes() const noexcept {
        return biomes_;
    }

private:
    std::map<data::ResourceLocation, std::vector<std::shared_ptr<const CompiledCarver>>> biomes_;
};

/// What the aquifer answers for a carved cell at density 0.0 (A-Q12.1).
struct AquiferAnswer {
    enum class Kind : std::uint8_t { Air, Water, Lava, Solid };
    Kind kind = Kind::Air;
    /// The aquifer's fluid-update flag for this decision (A-Q12.4).
    bool flag = false;
};

/// One target chunk's carving: where it is, and the generator's services.
/// Local coordinates are 0..15; y is a world y.
struct CarveTarget {
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    std::int64_t worldSeed = 0;
    settings::NoiseGeometry geometry;

    /// The biome the dimension's biome source holds at the quart whose lowest
    /// corner is block (x, 0, z) (A-Q11.2: quart (4 Sx, 0, 4 Sz)).
    std::function<const data::ResourceLocation&(std::int32_t x, std::int32_t z)> biomeAt;
    /// The block id at a target cell, and a write to it.
    std::function<const data::ResourceLocation&(int localX, std::int32_t y, int localZ)> blockAt;
    /// The kinds of state a carver writes.
    enum class Write : std::uint8_t { Air, CaveAir, Water, Lava };
    std::function<void(int localX, std::int32_t y, int localZ, Write state)> write;
    /// A write of a state a pack names: a carver's debug state (A-Q13.4).
    std::function<void(int localX, std::int32_t y, int localZ, const settings::BlockState& state)>
        writeState;
    /// The aquifer's answer at world (x, y, z), density 0.0 (A-Q12.1).
    std::function<AquiferAnswer(std::int32_t x, std::int32_t y, std::int32_t z)> aquifer;
    /// A-Q12.3's rewrite: set the dirt cell at (localX, y, localZ) to the
    /// surface rules' top material, "fluid above" set as given; returns
    /// whether the material it wrote holds fluid, for the unconditional mark.
    std::function<bool(int localX, std::int32_t y, int localZ, bool fluidAbove)> topMaterial;
    /// A post-processing mark (A-Q12.4).
    std::function<void(int localX, std::int32_t y, int localZ)> mark;
};

/// Carves @p target with @p carvers.
void carveChunk(const CarverSet& carvers, const CarveTarget& target);

/// What a world freezes of its carvers (SPEC §6), detached from the pack it
/// came from: every configured carver and every biome's `carvers` field as
/// JSON, as written, and every block tag a carver's `replaceable` names,
/// resolved to its blocks. A blob carries these rather than a CarverSet so
/// that a thawed world compiles its carvers through the same parse a pack's
/// do, refusals included.
struct CarverSources {
    std::map<data::ResourceLocation, nlohmann::json> configured;
    std::map<data::ResourceLocation, nlohmann::json> biomeCarvers;
    std::map<data::ResourceLocation, std::set<data::ResourceLocation>> tags;

    [[nodiscard]] bool empty() const noexcept { return biomeCarvers.empty(); }

    [[nodiscard]] bool operator==(const CarverSources&) const = default;
};

/// Gathers @p pack's carvers: its `worldgen/configured_carver` entries, each
/// biome's `carvers` field, and the tags they name, resolved through
/// @p tags. Every carver is parsed here, so a pack this build cannot carve
/// is refused at freeze time, by name (CarverError, data::BlockTagError),
/// not when a chunk first reaches it.
[[nodiscard]] CarverSources gatherCarverSources(const data::Pack& pack,
                                                const data::BlockTags& tags);

/// Every biome's carver list, compiled from @p sources: references resolved
/// among its configured carvers, inline definitions parsed in place, each
/// `replaceable` tag looked up in its resolved tags. Throws CarverError
/// naming the biome, the carver or the tag it cannot resolve.
[[nodiscard]] CarverSet buildCarverSet(const CarverSources& sources);

/// Every state a carver of @p carvers writes under `debug_mode`
/// (spec A-Q13.4), as it writes it, deduplicated in a stable order: the four
/// debug states of each cave or canyon whose `debug_mode` is set (the water
/// state waterlogged where it has the property). Empty for a set with no
/// such carver — every shipped one. For a binding that has to know a
/// dimension's block states before it fills a chunk.
[[nodiscard]] std::vector<settings::BlockState> debugStates(const CarverSet& carvers);

} // namespace stratum::carver
