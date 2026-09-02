// Stratum — resource locations.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The `namespace:path` identifiers every piece of worldgen data is addressed
// by. Parsing them strictly matters: a mistyped id that silently became a
// different valid id would resolve to the wrong density function and quietly
// change the world, so anything malformed is refused with the character and
// position that made it so.
//
// Character rules follow the datapack format as documented on
// minecraft.wiki: namespaces allow [a-z0-9._-], paths additionally allow '/'.

#pragma once

#include <compare>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace stratum::data {

/// Raised for a malformed identifier. The message names what was wrong.
class ResourceLocationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ResourceLocation {
public:
    /// The namespace assumed when an identifier carries none.
    static constexpr std::string_view kDefaultNamespace = "minecraft";

    /// Parses `namespace:path`, or a bare `path` in the default namespace.
    /// Throws ResourceLocationError if either half is malformed.
    [[nodiscard]] static ResourceLocation parse(std::string_view text);

    /// Same, but reports failure rather than throwing.
    [[nodiscard]] static std::optional<ResourceLocation> tryParse(std::string_view text);

    ResourceLocation(std::string namespaceName, std::string path);

    [[nodiscard]] const std::string& namespaceName() const noexcept { return namespace_; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// "minecraft:overworld", always fully qualified — an error message that
    /// dropped the namespace would be ambiguous between packs.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] bool operator==(const ResourceLocation& other) const = default;
    [[nodiscard]] std::strong_ordering operator<=>(const ResourceLocation& other) const = default;

private:
    std::string namespace_;
    std::string path_;
};

/// True when every character is legal for a namespace.
[[nodiscard]] bool isValidNamespace(std::string_view text) noexcept;

/// True when every character is legal for a path.
[[nodiscard]] bool isValidPath(std::string_view text) noexcept;

} // namespace stratum::data
