// Stratum — Java -> Bedrock biome mapping.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC section 9, the first of `lib/mapping/`'s two pieces: a biome id this
// build generated, translated to Bedrock's own numeric id for whatever a
// Bedrock-facing consumer (ext/, cli/) needs it for. Deliberately downstream
// of the conformance boundary — Tier-A parity is diffed in Java block space
// before anything reaches this library, and generation never calls into it.
//
// The table is generated from GeyserMC/mappings (MIT) at a pinned commit;
// see tools/mapping-sync for how, and for why that commit (the one
// GeyserMC/Geyser itself shipped for this build's pinned Java version).
//
// WHAT IS NOT HERE YET. SPEC section 9 also wants a fallback for biomes this
// table has no entry for — "custom/datapack biomes fall back to the nearest
// vanilla Bedrock biome" — which needs a similarity search over biome
// climate parameters this library does not implement. Shipping a fixed
// placeholder default in its place would look like that feature from the
// outside while being a different, much weaker one; `bedrockBiomeId` returns
// nothing instead; a real fallback is PROGRESS.md's next `lib/mapping/` slice.
#pragma once

#include <stratum/data/resource_location.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace stratum::mapping {

/// Bedrock's numeric id for @p javaBiome, or nothing when this build's
/// generated table has no direct entry for it — every vanilla biome at the
/// pinned version resolves; a custom or datapack biome does not.
[[nodiscard]] std::optional<std::int32_t>
bedrockBiomeId(const data::ResourceLocation& javaBiome) noexcept;

/// How many biomes the generated table covers, for tests that want to
/// assert full coverage without hard-coding the count twice.
[[nodiscard]] std::size_t biomeTableSize() noexcept;

} // namespace stratum::mapping
