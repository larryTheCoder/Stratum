// Stratum — checking a pack without generating from it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// What `stratum validate` is built on. It answers one question — "would this
// pack work here, and if not, exactly what about it would not?" — and it
// answers it by doing everything world load does short of sampling a point:
// open, resolve every reference, build every noise from a seed, and walk
// each density function for node types this build cannot execute.
//
// TWO KINDS OF NO. SPEC §8 makes the capability matrix a public contract,
// and this report keeps its two halves apart, because they mean different
// things to whoever is holding the pack:
//
//   * a registry v1 does not execute — `placed_feature` and its neighbours.
//     The pack is fine; this engine is not going to run that part of it, by
//     design, and vanilla's own data is full of them.
//   * a density function this *build* cannot yet evaluate. The pack is fine
//     and the engine intends to run it, but the code is not there yet
//     (SPEC §10, M3).
//
// Neither is silently swallowed, and neither is reported as the other.
//
// WHAT A CLEAN REPORT DOES NOT MEAN. Five of the eight registries v1 executes
// are loaded and addressable but not yet interpreted — biomes, dimensions and
// the rest are parsed as JSON and nothing more — and the surface rules inside
// a noise settings entry are read but not understood. A pack whose surface
// rules are nonsense will validate clean today. The report says so itself
// rather than leaving the silence to be read as approval.

#pragma once

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::validate {

enum class Severity : std::uint8_t {
    /// Worth knowing; not a problem with the pack.
    Note,
    /// The pack loads, but some of it will not be executed here.
    Warning,
    /// The pack cannot be loaded or resolved at all.
    Error,
};

[[nodiscard]] std::string_view severityName(Severity severity) noexcept;

struct Finding {
    Severity severity = Severity::Note;
    /// What the finding is about — an identifier, a registry directory — or
    /// empty when it is about the pack as a whole.
    std::string subject;
    std::string message;
};

struct RegistryCount {
    data::Registry registry;
    std::size_t entries = 0;
    /// Whether this build executes the registry at all (SPEC §8).
    bool supported = false;
    /// Whether this build interprets its contents *yet*, as opposed to
    /// merely loading them. The distinction is the difference between a
    /// clean report that means something and one that does not.
    bool interpreted = false;
};

struct Report {
    /// Every registry the pack has entries in, in a stable order.
    std::vector<RegistryCount> registries;
    std::vector<Finding> findings;

    std::size_t densityFunctions = 0;
    /// Of those, how many this build could evaluate at a point today.
    std::size_t evaluable = 0;
    std::size_t noisesReferenced = 0;
    std::size_t nodes = 0;
    std::size_t splines = 0;
    std::size_t noiseSettings = 0;
    /// Router entries across every noise settings entry, and how many of
    /// them this build could evaluate. Counted rather than only listed:
    /// seven dimensions times fifteen entries is a lot of warnings, and
    /// "94 of 105" is the number someone actually wants.
    std::size_t routerEntries = 0;
    std::size_t routerEntriesEvaluable = 0;

    /// False when the pack could not be resolved at all, in which case the
    /// counts above are whatever had been established when it stopped.
    bool resolved = false;

    [[nodiscard]] std::size_t count(Severity severity) const noexcept;

    [[nodiscard]] bool clean() const noexcept {
        return count(Severity::Error) == 0 && count(Severity::Warning) == 0;
    }
};

struct ValidateOptions {
    /// Noises are built as part of validating, because a `firstOctave` that
    /// is a string is a broken pack whether or not anyone generates from it.
    /// Which seed hardly matters; that it is fixed does.
    std::int64_t seed = 0;
};

/// Checks @p pack. Never throws for a problem *with the pack* — that is what
/// the findings are for — so a caller gets the whole picture rather than the
/// first thing that went wrong.
[[nodiscard]] Report validatePack(const data::Pack& pack, const ValidateOptions& options = {});

} // namespace stratum::validate
