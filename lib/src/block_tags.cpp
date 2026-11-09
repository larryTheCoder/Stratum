// Stratum — block tags (data/block_tags.hpp).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/block_tags.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stratum::data {

namespace {

/// An id as a tag value spells it: `minecraft:stone` or `#minecraft:dirt`,
/// refused by name when malformed or outside the `minecraft` namespace.
[[nodiscard]] BlockTagEntry entryFrom(const std::string& text, const bool required,
                                      const ResourceLocation& tag) {
    BlockTagEntry entry{
        .id = ResourceLocation{"minecraft", "air"}, .isTag = false, .required = required};
    std::string_view spelled = text;
    if (spelled.starts_with('#')) {
        entry.isTag = true;
        spelled.remove_prefix(1);
    }
    const std::optional<ResourceLocation> id = ResourceLocation::tryParse(spelled);
    if (!id.has_value()) {
        throw BlockTagError("block tag " + tag.toString() + " names '" + text +
                            "', which is not a resource location");
    }
    if (entry.isTag && id->namespaceName() != "minecraft") {
        throw BlockTagError("block tag " + tag.toString() + " references #" + id->toString() +
                            ", outside the minecraft namespace this build reads tags from");
    }
    entry.id = *id;
    return entry;
}

[[nodiscard]] std::vector<BlockTagEntry> parseTag(const nlohmann::json& json,
                                                  const ResourceLocation& tag) {
    if (!json.is_object()) {
        throw BlockTagError("block tag " + tag.toString() + " is not a JSON object");
    }
    for (const auto& [key, unused] : json.items()) {
        if (key != "values" && key != "replace") {
            throw BlockTagError("block tag " + tag.toString() + " has field '" + key +
                                "', which a tag file does not have");
        }
    }
    if (!json.contains("values") || !json.at("values").is_array()) {
        throw BlockTagError("block tag " + tag.toString() + " has no \"values\" array");
    }
    std::vector<BlockTagEntry> entries;
    entries.reserve(json.at("values").size());
    for (const nlohmann::json& value : json.at("values")) {
        if (value.is_string()) {
            entries.push_back(entryFrom(value.get<std::string>(), true, tag));
        } else if (value.is_object() && value.contains("id") && value.at("id").is_string()) {
            const bool required = !value.contains("required") || value.at("required").get<bool>();
            entries.push_back(entryFrom(value.at("id").get<std::string>(), required, tag));
        } else {
            throw BlockTagError("block tag " + tag.toString() + " has a value " + value.dump() +
                                R"( that is neither an id nor {"id": ..., "required": ...})");
        }
    }
    return entries;
}

void resolveInto(const std::map<ResourceLocation, std::vector<BlockTagEntry>>& tags,
                 const ResourceLocation& tag, std::vector<ResourceLocation>& chain,
                 std::set<ResourceLocation>& out) {
    if (std::ranges::find(chain, tag) != chain.end()) {
        std::string path;
        for (const ResourceLocation& link : chain) {
            path += "#" + link.toString() + " -> ";
        }
        throw BlockTagError("block tags reference each other in a cycle: " + path + "#" +
                            tag.toString());
    }
    const auto found = tags.find(tag);
    if (found == tags.end()) {
        throw BlockTagError("no block tag #" + tag.toString());
    }
    chain.push_back(tag);
    for (const BlockTagEntry& entry : found->second) {
        if (!entry.isTag) {
            out.insert(entry.id);
        } else if (tags.contains(entry.id)) {
            resolveInto(tags, entry.id, chain, out);
        } else if (entry.required) {
            throw BlockTagError("block tag #" + tag.toString() + " references #" +
                                entry.id.toString() + ", which does not exist");
        }
    }
    chain.pop_back();
}

} // namespace

BlockTags BlockTags::load(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw BlockTagError("no block tag directory at " + directory.string());
    }
    std::map<ResourceLocation, std::vector<BlockTagEntry>> tags;
    for (const auto& file : std::filesystem::recursive_directory_iterator(directory)) {
        if (!file.is_regular_file() || file.path().extension() != ".json") {
            continue;
        }
        std::string path = file.path().lexically_relative(directory).replace_extension().string();
        std::ranges::replace(path, '\\', '/');
        const std::optional<ResourceLocation> id = ResourceLocation::tryParse("minecraft:" + path);
        if (!id.has_value()) {
            throw BlockTagError("block tag file " + file.path().string() +
                                " does not name a resource location");
        }
        std::ifstream in(file.path());
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(in);
        } catch (const nlohmann::json::exception& error) {
            throw BlockTagError("block tag file " + file.path().string() +
                                " is not JSON: " + error.what());
        }
        tags.emplace(*id, parseTag(json, *id));
    }
    return fromEntries(std::move(tags));
}

BlockTags BlockTags::fromEntries(std::map<ResourceLocation, std::vector<BlockTagEntry>> tags) {
    BlockTags result;
    result.tags_ = std::move(tags);
    return result;
}

std::set<ResourceLocation> BlockTags::resolve(const ResourceLocation& tag) const {
    std::set<ResourceLocation> out;
    std::vector<ResourceLocation> chain;
    resolveInto(tags_, tag, chain, out);
    return out;
}

} // namespace stratum::data
