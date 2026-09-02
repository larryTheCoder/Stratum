// Stratum — worldgen data packs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Finds the worldgen entries in a pack and parses them. This layer does not
// interpret a density function or a noise; it establishes what exists, under
// which identifier, and refuses anything it cannot account for.
//
// Two layouts are read, because both are real inputs:
//   * a data pack — `pack.mcmeta` beside `data/<namespace>/worldgen/...`,
//     which is what a user supplies;
//   * an extracted registry tree — `worldgen/density_function/...` with no
//     metadata, which is what tools/fetch-vanilla pulls out of the jar.

#pragma once

#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::data {

/// Raised when a pack cannot be read, or declares itself incompatible.
class PackError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One JSON file, located and identified but not yet interpreted.
struct PackEntry {
    Registry registry;
    ResourceLocation id;
    std::filesystem::path file;
    nlohmann::json json;
};

/// An entry in a registry this build does not execute (SPEC §8). Recorded
/// rather than dropped, so a caller can report exactly what it will ignore.
struct RejectedEntry {
    Registry registry;
    ResourceLocation id;
    std::filesystem::path file;
};

struct PackLoadOptions {
    /// Refuse the pack outright if it carries entries in registries v1 does
    /// not execute. Off by default: vanilla's own data ships hundreds of
    /// them, and the Tier-A goldens are generated with features stripped
    /// precisely because they are out of scope. A caller loading a
    /// third-party pack that must be honoured in full should turn it on.
    bool rejectUnsupported = false;

    /// Refuse a directory under `worldgen/` that this build does not know
    /// at all — as opposed to one it knows and does not execute. On by
    /// default: an unknown registry means the pack expects something of us
    /// that we cannot even name.
    bool rejectUnknownDirectories = true;
};

class Pack {
public:
    /// Reads a data pack: `pack.mcmeta` plus `data/<namespace>/...`. The
    /// declared pack format must cover the pinned one (SPEC §3).
    [[nodiscard]] static Pack openDataPack(const std::filesystem::path& root,
                                           const PackLoadOptions& options = {});

    /// Reads an extracted worldgen tree — the shape tools/fetch-vanilla
    /// pulls out of the jar, rooted at `worldgen/` itself so its entries are
    /// `density_function/...` rather than `worldgen/density_function/...`.
    /// It carries no metadata, so the namespace has to be supplied.
    [[nodiscard]] static Pack openWorldgenTree(const std::filesystem::path& root,
                                               std::string_view namespaceName = "minecraft",
                                               const PackLoadOptions& options = {});

    [[nodiscard]] const std::vector<PackEntry>& entries() const noexcept { return entries_; }

    [[nodiscard]] const std::vector<RejectedEntry>& rejected() const noexcept { return rejected_; }

    /// Entries of one registry, in a stable order.
    [[nodiscard]] std::vector<const PackEntry*> entriesOf(Registry registry) const;

    /// The entry with this identifier, or nullptr.
    [[nodiscard]] const PackEntry* find(Registry registry, const ResourceLocation& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    /// @p directoryPrefix is prepended to each file's path before it is
    /// matched against a registry directory, so a tree rooted inside
    /// `worldgen/` still resolves to the right registries.
    void scan(const std::filesystem::path& root, const std::string& namespaceName,
              const PackLoadOptions& options, std::string_view directoryPrefix);

    std::vector<PackEntry> entries_;
    std::vector<RejectedEntry> rejected_;
    std::map<std::pair<Registry, ResourceLocation>, std::size_t> index_;
};

/// Validates a `pack.mcmeta` against the pinned data pack format (SPEC §3).
/// Throws PackError naming the declared range when it does not cover ours.
void validatePackFormat(const nlohmann::json& mcmeta, const std::filesystem::path& file);

} // namespace stratum::data
