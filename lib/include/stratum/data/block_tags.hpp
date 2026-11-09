// Stratum — block tags: what `#namespace:path` names as a set of blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A configured carver's `replaceable` is a block set, written as a tag
// (`#minecraft:overworld_carver_replaceables`), a single block, or a list
// (spec/carvers-a-spec.md Q12.2). Tags reference tags, so a tag is resolved
// through the whole tree it reaches.
//
// THE FILE FORMAT (runtime data, minecraft.wiki "Tag"): `{"replace": bool,
// "values": [...]}`, each value a block id, a `#tag`, or `{"id": ...,
// "required": bool}` where a missing optional entry is skipped. Loaded from
// a directory laid out as the data pack's `tags/block/` (tools/fetch-vanilla
// extracts vanilla's to `<version>/tags/block`), `minecraft` namespace only:
// a reference to another namespace is refused by name, as is a cycle, a
// missing required tag, or anything this format does not name (SPEC §8).

#pragma once

#include <stratum/data/resource_location.hpp>

#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::data {

class BlockTagError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One entry of a tag file's `values`, as written.
struct BlockTagEntry {
    /// A block id, or (when @ref isTag) a tag's id without the `#`.
    ResourceLocation id;
    bool isTag = false;
    bool required = true;

    [[nodiscard]] bool operator==(const BlockTagEntry&) const = default;
};

class BlockTags {
public:
    /// Reads every `*.json` under @p directory (the `tags/block` directory of
    /// the `minecraft` namespace), parsing each, resolving nothing yet.
    /// Throws BlockTagError naming the file for one that does not parse.
    [[nodiscard]] static BlockTags load(const std::filesystem::path& directory);

    /// The same, from entries already parsed (tests; a frozen pipeline).
    [[nodiscard]] static BlockTags
    fromEntries(std::map<ResourceLocation, std::vector<BlockTagEntry>> tags);

    /// Every block @p tag names, through every tag it references. Throws
    /// BlockTagError naming the chain for a cycle, and the tag for a required
    /// one that does not exist.
    [[nodiscard]] std::set<ResourceLocation> resolve(const ResourceLocation& tag) const;

    [[nodiscard]] bool contains(const ResourceLocation& tag) const noexcept {
        return tags_.contains(tag);
    }

    [[nodiscard]] const std::map<ResourceLocation, std::vector<BlockTagEntry>>&
    entries() const noexcept {
        return tags_;
    }

private:
    std::map<ResourceLocation, std::vector<BlockTagEntry>> tags_;
};

} // namespace stratum::data
