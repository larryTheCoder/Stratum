// Stratum — worldgen data packs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/pack.hpp>
#include <stratum/version.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stratum::data {

namespace {

[[nodiscard]] nlohmann::json readJson(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw PackError("cannot open " + file.string());
    }
    try {
        return nlohmann::json::parse(stream);
    } catch (const nlohmann::json::parse_error& error) {
        // The library's message carries the byte offset; the file name is
        // what turns it into something you can act on.
        throw PackError(file.string() + ": " + error.what());
    }
}

/// `worldgen/density_function/overworld/base_3d_noise.json` relative to a
/// namespace root becomes ("worldgen/density_function",
/// "overworld/base_3d_noise").
struct SplitPath {
    Registry registry;
    std::string entryPath;
};

[[nodiscard]] std::optional<SplitPath> splitRegistryPath(const std::filesystem::path& relative) {
    std::vector<std::string> parts;
    for (const std::filesystem::path& part : relative) {
        parts.push_back(part.generic_string());
    }
    if (parts.size() < 2) {
        return std::nullopt;
    }

    // Registry directories are one or two segments deep ("dimension",
    // "worldgen/noise"), so try the longer spelling first.
    for (const std::size_t depth : {std::size_t{2}, std::size_t{1}}) {
        if (parts.size() <= depth) {
            continue;
        }
        std::string directory = parts[0];
        for (std::size_t i = 1; i < depth; ++i) {
            directory += "/" + parts[i];
        }
        const std::optional<Registry> registry = registryFromDirectory(directory);
        if (!registry.has_value()) {
            continue;
        }
        std::string entryPath = parts[depth];
        for (std::size_t i = depth + 1; i < parts.size(); ++i) {
            entryPath += "/" + parts[i];
        }
        return SplitPath{.registry = *registry, .entryPath = std::move(entryPath)};
    }
    return std::nullopt;
}

[[nodiscard]] std::string stripJsonSuffix(std::string path) {
    static constexpr std::string_view kSuffix = ".json";
    if (path.size() > kSuffix.size() && path.ends_with(kSuffix)) {
        path.resize(path.size() - kSuffix.size());
    }
    return path;
}

} // namespace

void validatePackFormat(const nlohmann::json& mcmeta, const std::filesystem::path& file) {
    if (!mcmeta.contains("pack")) {
        throw PackError(file.string() + ": no \"pack\" object; this is not a pack.mcmeta");
    }
    const nlohmann::json& pack = mcmeta.at("pack");

    // A format is declared either as a single pack_format or as a
    // min_format/max_format range, and either half of the range may itself be
    // a [major, minor] pair.
    const auto majorOf = [&file](const nlohmann::json& value) -> int {
        if (value.is_number_integer()) {
            return value.get<int>();
        }
        if (value.is_array() && !value.empty() && value.at(0).is_number_integer()) {
            return value.at(0).get<int>();
        }
        throw PackError(file.string() + ": pack format " + value.dump() +
                        " is neither a number nor a [major, minor] pair");
    };

    std::optional<int> minimum;
    std::optional<int> maximum;
    if (pack.contains("pack_format")) {
        minimum = majorOf(pack.at("pack_format"));
        maximum = minimum;
    }
    if (pack.contains("min_format")) {
        minimum = majorOf(pack.at("min_format"));
    }
    if (pack.contains("max_format")) {
        maximum = majorOf(pack.at("max_format"));
    }

    if (!minimum.has_value() || !maximum.has_value()) {
        throw PackError(file.string() +
                        ": declares no pack format; expected pack_format, or min_format and "
                        "max_format");
    }
    if (kPackFormatMajor < *minimum || kPackFormatMajor > *maximum) {
        throw PackError(file.string() + ": declares support for pack format " +
                        std::to_string(*minimum) + ".." + std::to_string(*maximum) +
                        ", which does not cover the pinned format " +
                        std::to_string(kPackFormatMajor) + "." + std::to_string(kPackFormatMinor) +
                        " (SPEC §3). Refusing rather than loading it partly.");
    }
}

Pack Pack::open(const std::filesystem::path& root, const PackLoadOptions& options) {
    if (std::filesystem::is_regular_file(root / "pack.mcmeta")) {
        return openDataPack(root, options);
    }
    if (std::filesystem::is_directory(root / "density_function")) {
        return openWorldgenTree(root, ResourceLocation::kDefaultNamespace, options);
    }
    if (std::filesystem::is_directory(root / "worldgen" / "density_function")) {
        return openWorldgenTree(root / "worldgen", ResourceLocation::kDefaultNamespace, options);
    }
    throw PackError("no pack at '" + root.string() +
                    "': expected pack.mcmeta beside data/, a worldgen tree containing "
                    "density_function/, or a directory containing worldgen/density_function/");
}

Pack Pack::openDataPack(const std::filesystem::path& root, const PackLoadOptions& options) {
    const std::filesystem::path mcmeta = root / "pack.mcmeta";
    if (!std::filesystem::is_regular_file(mcmeta)) {
        throw PackError(root.string() + ": no pack.mcmeta, so this is not a data pack");
    }
    validatePackFormat(readJson(mcmeta), mcmeta);

    const std::filesystem::path data = root / "data";
    if (!std::filesystem::is_directory(data)) {
        throw PackError(root.string() + ": has pack.mcmeta but no data/ directory");
    }

    Pack pack;
    pack.layout_ = Layout::DataPack;
    std::vector<std::filesystem::path> namespaces;
    for (const auto& entry : std::filesystem::directory_iterator(data)) {
        if (entry.is_directory()) {
            namespaces.push_back(entry.path());
        }
    }
    // Directory order is unspecified; a pack must load the same way twice.
    std::ranges::sort(namespaces);

    for (const std::filesystem::path& namespaceRoot : namespaces) {
        const std::string namespaceName = namespaceRoot.filename().string();
        if (!isValidNamespace(namespaceName)) {
            throw PackError(namespaceRoot.string() + ": '" + namespaceName +
                            "' is not a valid namespace");
        }
        pack.scan(namespaceRoot, namespaceName, options, "");
    }
    return pack;
}

Pack Pack::openWorldgenTree(const std::filesystem::path& root, std::string_view namespaceName,
                            const PackLoadOptions& options) {
    if (!std::filesystem::is_directory(root)) {
        throw PackError(root.string() + ": not a directory");
    }
    if (!isValidNamespace(namespaceName)) {
        throw PackError("'" + std::string(namespaceName) + "' is not a valid namespace");
    }

    Pack pack;
    pack.layout_ = Layout::WorldgenTree;
    // Rooted inside worldgen/, so the prefix restores what the layout drops.
    pack.scan(root, std::string(namespaceName), options, "worldgen/");
    return pack;
}

void Pack::scan(const std::filesystem::path& root, const std::string& namespaceName,
                const PackLoadOptions& options, std::string_view directoryPrefix) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    for (const std::filesystem::path& file : files) {
        const std::filesystem::path relative =
            std::filesystem::path(directoryPrefix) / std::filesystem::relative(file, root);
        const std::optional<SplitPath> split = splitRegistryPath(relative);

        if (!split.has_value()) {
            if (options.rejectUnknownDirectories) {
                throw PackError(file.string() +
                                ": is not under a registry directory this build knows. Refusing "
                                "rather than ignoring it (SPEC §8).");
            }
            continue;
        }

        const Registry registry = split->registry;
        ResourceLocation id{namespaceName, stripJsonSuffix(split->entryPath)};

        if (!isSupported(registry)) {
            if (options.rejectUnsupported) {
                throw PackError(file.string() + ": " + id.toString() + " is in registry '" +
                                std::string(registryDirectory(registry)) +
                                "', which this engine does not execute (SPEC §8).");
            }
            rejected_.push_back(
                RejectedEntry{.registry = registry, .id = std::move(id), .file = file});
            continue;
        }

        const auto key = std::pair{registry, id};
        if (index_.contains(key)) {
            throw PackError(file.string() + ": " + id.toString() + " is defined twice in '" +
                            std::string(registryDirectory(registry)) + "'");
        }
        index_.emplace(key, entries_.size());
        entries_.push_back(PackEntry{
            .registry = registry, .id = std::move(id), .file = file, .json = readJson(file)});
    }
}

std::vector<const PackEntry*> Pack::entriesOf(Registry registry) const {
    std::vector<const PackEntry*> found;
    for (const PackEntry& entry : entries_) {
        if (entry.registry == registry) {
            found.push_back(&entry);
        }
    }
    return found;
}

const PackEntry* Pack::find(Registry registry, const ResourceLocation& id) const {
    const auto found = index_.find(std::pair{registry, id});
    return found == index_.end() ? nullptr : &entries_[found->second];
}

} // namespace stratum::data
