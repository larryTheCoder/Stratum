// Stratum — what a probe corpus records about how it was generated.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A probe world's fluids keep flowing after generation unless the world is
// frozen first, and how far they get before the save depends on wall-clock
// timing (SPEC §7: two unfrozen runs of one golden seed differed in 197
// blocks). On a probe the amount is large: one water/lava arm differs by
// 11 724 blocks between a frozen and an unfrozen run of the same seed. A count
// measured on an unfrozen corpus is not reproducible, so a case that scores
// fluid refuses one rather than reporting the difference as a parity
// regression. The probe harnesses freeze the world (`/tick freeze`) before
// any chunk generates and record that in the corpus's manifest.json as
// `ticks_frozen`.
#pragma once

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace stratum::test {

/// Whether the probe corpus in @p dir was generated with ticks frozen, read
/// from its manifest.json. A corpus with no manifest, or with a manifest
/// written before the field existed, was not.
[[nodiscard]] inline bool ticksFrozen(const std::filesystem::path& dir) {
    std::ifstream in(dir / "manifest.json");
    if (!in) {
        return false;
    }
    const nlohmann::json manifest = nlohmann::json::parse(in, nullptr, false);
    return !manifest.is_discarded() && manifest.is_object() &&
           manifest.value("ticks_frozen", false);
}

/// Fails the running case unless the corpus in @p dir was generated from
/// @p seed. A case that knows its corpus's seed rather than reading it
/// would otherwise score another seed's world against its own centres and
/// noises, and report that as disagreement.
inline void requireSeed(const std::filesystem::path& dir, std::int64_t seed) {
    std::ifstream in(dir / "manifest.json");
    INFO(dir << " has no readable manifest.json; regenerate it");
    REQUIRE(in.good());
    const nlohmann::json manifest = nlohmann::json::parse(in, nullptr, false);
    REQUIRE(!manifest.is_discarded());
    REQUIRE(manifest.contains("seed"));
    INFO(dir << " was generated from seed " << manifest.at("seed") << ", not " << seed);
    REQUIRE(manifest.at("seed").get<std::int64_t>() == seed);
}

/// Fails the running case unless the corpus in @p dir was generated frozen,
/// naming the script that regenerates it.
inline void requireFrozen(const std::filesystem::path& dir, std::string_view regenerate) {
    INFO(dir << " was generated before probe worlds were frozen (its manifest records no "
                "ticks_frozen); regenerate it with "
             << regenerate);
    REQUIRE(ticksFrozen(dir));
}

} // namespace stratum::test
