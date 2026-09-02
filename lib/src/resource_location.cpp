// Stratum — resource locations.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/resource_location.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace stratum::data {

namespace {

[[nodiscard]] bool isNamespaceCharacter(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
}

[[nodiscard]] bool isPathCharacter(char character) noexcept {
    return isNamespaceCharacter(character) || character == '/';
}

/// Names the first offending character and where it is, because "invalid
/// identifier" alone does not tell you which of 952 files to look at.
[[noreturn]] void reject(std::string_view text, std::string_view part, std::size_t position) {
    throw ResourceLocationError("invalid character '" + std::string(1, text[position]) +
                                "' at position " + std::to_string(position) + " of " +
                                std::string(part) + " in resource location '" + std::string(text) +
                                "'");
}

} // namespace

bool isValidNamespace(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, isNamespaceCharacter);
}

bool isValidPath(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, isPathCharacter);
}

ResourceLocation::ResourceLocation(std::string namespaceName, std::string path)
    : namespace_(std::move(namespaceName)), path_(std::move(path)) {
    if (namespace_.empty()) {
        throw ResourceLocationError("resource location has an empty namespace");
    }
    if (path_.empty()) {
        throw ResourceLocationError("resource location '" + namespace_ + ":' has an empty path");
    }
    for (std::size_t i = 0; i < namespace_.size(); ++i) {
        if (!isNamespaceCharacter(namespace_[i])) {
            reject(namespace_, "the namespace", i);
        }
    }
    for (std::size_t i = 0; i < path_.size(); ++i) {
        if (!isPathCharacter(path_[i])) {
            reject(path_, "the path", i);
        }
    }
}

ResourceLocation ResourceLocation::parse(std::string_view text) {
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos) {
        // A bare path is in the default namespace, as vanilla reads it.
        return ResourceLocation{std::string(kDefaultNamespace), std::string(text)};
    }
    if (text.find(':', separator + 1) != std::string_view::npos) {
        throw ResourceLocationError("resource location '" + std::string(text) +
                                    "' has more than one ':'");
    }
    return ResourceLocation{std::string(text.substr(0, separator)),
                            std::string(text.substr(separator + 1))};
}

std::optional<ResourceLocation> ResourceLocation::tryParse(std::string_view text) {
    try {
        return parse(text);
    } catch (const ResourceLocationError&) {
        return std::nullopt;
    }
}

std::string ResourceLocation::toString() const {
    return namespace_ + ":" + path_;
}

} // namespace stratum::data
