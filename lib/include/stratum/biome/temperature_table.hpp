// Stratum — a biome's own declared temperature.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `minecraft:temperature` (surface rules, SPEC §11) does not read the
// climate noise that picked a biome — it reads that biome's own DECLARED
// `temperature`, the constant every `worldgen/biome/*.json` writes once and
// `surface::Executor::freezing` then adjusts by height at read time. This is
// that value, keyed by the biome's identifier and read from the same
// `worldgen/biome` entries `data::Pack` already locates and parses, so
// nothing here re-walks a directory `Pack::open` has already walked.

#pragma once

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>

#include <cstddef>
#include <map>
#include <stdexcept>

namespace stratum::biome {

/// Raised when a biome's declared temperature cannot be read or found.
class TemperatureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Every biome a pack defines, mapped to its own declared `temperature`.
/// Built once from `data::Pack::entriesOf(Registry::Biome)` rather than
/// searched per lookup.
class TemperatureTable {
public:
    /// Reads every `worldgen/biome` entry @p pack holds. Throws
    /// TemperatureError, naming the biome, for an entry missing
    /// `temperature` or carrying one that is not a number — every real
    /// biome vanilla ships declares one, so this is a malformed pack, not a
    /// documented absence.
    [[nodiscard]] static TemperatureTable fromPack(const data::Pack& pack);

    /// @p id's declared temperature. Float32 on purpose, matching how
    /// vanilla stores and compares it (surface::Executor::freezing's own
    /// doc) — widening it here would change the answer there.
    ///
    /// Throws TemperatureError naming @p id when the pack this table was
    /// built from carried no `worldgen/biome` entry for it — reachable
    /// whenever a biome parameter list names a biome outside that pack —
    /// refused rather than defaulted (SPEC §8).
    [[nodiscard]] float at(const data::ResourceLocation& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return temperatures_.size(); }

private:
    std::map<data::ResourceLocation, float> temperatures_;
};

} // namespace stratum::biome
