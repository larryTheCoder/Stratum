// Stratum — what the decoration stage's translation units share.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stratum/feature/feature.hpp>

#include <string>

namespace stratum::feature::detail {

/// Compiles the placed feature @p json, named @p id in every refusal, its
/// configured feature and tags looked up in @p sources. Throws FeatureError
/// for malformed data and Unsupported for data this build does not run.
[[nodiscard]] PlacedFeature compilePlaced(const nlohmann::json& json, const std::string& id,
                                          const FeatureSources& sources);

/// How a block counts for the heightmaps and the fluid predicates. MEASURED
/// for the terrain's blocks (cli/src/generate.cpp, heightClassOf: every
/// column of sixteen golden regions): air; the two fluids; powder snow, which
/// is present and passable; everything else blocks motion.
enum class BlockClass : std::uint8_t { Air, Water, Lava, Passable, Blocking };

[[nodiscard]] BlockClass classOf(const settings::BlockState& state) noexcept;

/// Whether @p state is a leaf block, which MOTION_BLOCKING_NO_LEAVES ignores.
[[nodiscard]] bool isLeaves(const settings::BlockState& state) noexcept;

} // namespace stratum::feature::detail
