// Stratum — Java -> Bedrock biome mapping.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/mapping/biome.hpp>

#include <array>
#include <string_view>

namespace stratum::mapping {

namespace {

/// One row of the generated table: a Java biome id, verbatim, and the
/// Bedrock numeric id GeyserMC/mappings gives it. `javaId` is kept as a
/// string rather than a parsed `ResourceLocation` so the generated table is
/// plain data, not a table of objects with constructors to run at load time.
struct BiomeMapping {
    std::string_view javaId;
    std::int32_t bedrockId;
};

// The table itself, generated from GeyserMC/mappings (MIT) at a pinned
// commit. Hand-editing it would defeat the point: the mapping is
// authoritative about which biomes exist and what Bedrock calls them.
#include "biome_table.inc"

} // namespace

std::optional<std::int32_t> bedrockBiomeId(const data::ResourceLocation& javaBiome) noexcept {
    const std::string javaId = javaBiome.toString();
    for (const BiomeMapping& row : kBiomeTable) {
        if (row.javaId == javaId) {
            return row.bedrockId;
        }
    }
    return std::nullopt;
}

std::size_t biomeTableSize() noexcept {
    return kBiomeTable.size();
}

} // namespace stratum::mapping
