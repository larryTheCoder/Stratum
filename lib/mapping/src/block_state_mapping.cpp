// Stratum — Java block state -> Bedrock blockstate.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/mapping/block_state.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace stratum::mapping {

namespace {

// Everything below indexes into kStrings, so the generated table is plain
// integers and string literals with nothing to construct at load time.

/// One Java block. Its states are ids [firstState, firstState + product of
/// its properties' value counts), numbered mixed-radix over its properties
/// in the order listed — tools/mapping-sync verified that order against
/// every id vanilla's own report states.
struct JavaBlockRow {
    std::uint16_t name;
    std::uint16_t firstProperty;
    std::uint8_t propertyCount;
    std::uint32_t firstState;
    std::uint32_t defaultOrdinal;
};

struct JavaPropertyRow {
    std::uint16_t name;
    std::uint16_t firstValue;
    std::uint8_t valueCount;
};

struct BedrockValueRow {
    std::uint16_t key;
    BedrockTagType type;
    std::int32_t integer;
    std::uint16_t string;
};

#include "block_state_table.inc"

[[nodiscard]] std::string inQuotes(std::string_view text) {
    return "'" + std::string(text) + "'";
}

[[nodiscard]] std::string_view blockNameByRank(std::size_t rank) {
    return kStrings.at(kJavaBlocks.at(kJavaBlocksByName.at(rank)).name);
}

// A binary search by index, not std::lower_bound: an iterator into
// std::array is a raw pointer on one standard library and a class on
// another, and no spelling of its type satisfies both compilers and
// clang-tidy's readability-qualified-auto.
[[nodiscard]] const JavaBlockRow& findBlock(const std::string& name) {
    std::size_t low = 0;
    std::size_t high = kJavaBlocksByName.size();
    while (low < high) {
        const std::size_t middle = low + ((high - low) / 2);
        if (blockNameByRank(middle) < name) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == kJavaBlocksByName.size() || blockNameByRank(low) != name) {
        throw BlockMappingError("stratum-mapping: " + inQuotes(name) +
                                " is not a block at Minecraft " + std::string(kMinecraftVersion));
    }
    return kJavaBlocks.at(kJavaBlocksByName.at(low));
}

[[nodiscard]] std::string valueList(const JavaPropertyRow& property) {
    std::string list;
    for (std::size_t value = 0; value < property.valueCount; ++value) {
        list += (value == 0 ? "" : ", ");
        list += kStrings.at(kJavaPropertyValues.at(property.firstValue + value));
    }
    return list;
}

} // namespace

std::uint32_t javaBlockStateId(const settings::BlockState& state) {
    const std::string name = state.name.toString();
    const JavaBlockRow& block = findBlock(name);
    const auto properties =
        std::span(kJavaProperties).subspan(block.firstProperty, block.propertyCount);

    for (const auto& [key, value] : state.properties) {
        const bool known = std::ranges::any_of(properties, [&key](const JavaPropertyRow& row) {
            return kStrings.at(row.name) == key;
        });
        if (!known) {
            std::string names;
            for (const JavaPropertyRow& row : properties) {
                names += (names.empty() ? "" : ", ");
                names += kStrings.at(row.name);
            }
            throw BlockMappingError("stratum-mapping: " + inQuotes(name) + " has no property " +
                                    inQuotes(key) +
                                    " (it has: " + (names.empty() ? "none" : names) + ")");
        }
    }

    // Split the default ordinal into one digit per property, last property
    // least significant, so an omitted property can take its default digit.
    std::vector<std::uint32_t> defaultDigits(properties.size());
    std::uint32_t remainder = block.defaultOrdinal;
    for (std::size_t index = properties.size(); index-- > 0;) {
        defaultDigits[index] = remainder % properties[index].valueCount;
        remainder /= properties[index].valueCount;
    }

    std::uint32_t ordinal = 0;
    for (std::size_t index = 0; index < properties.size(); ++index) {
        const JavaPropertyRow& property = properties[index];
        std::uint32_t digit = defaultDigits[index];
        if (const auto given = state.properties.find(std::string(kStrings.at(property.name)));
            given != state.properties.end()) {
            const auto values =
                std::span(kJavaPropertyValues).subspan(property.firstValue, property.valueCount);
            const auto match = std::ranges::find_if(values, [&given](std::uint16_t valueName) {
                return kStrings.at(valueName) == given->second;
            });
            if (match == values.end()) {
                throw BlockMappingError("stratum-mapping: " + inQuotes(name) + " property " +
                                        inQuotes(given->first) + " has no value " +
                                        inQuotes(given->second) +
                                        " (it has: " + valueList(property) + ")");
            }
            digit = static_cast<std::uint32_t>(match - values.begin());
        }
        ordinal = ordinal * property.valueCount + digit;
    }
    return block.firstState + ordinal;
}

BedrockBlockState bedrockBlockState(const std::uint32_t javaStateId) {
    if (javaStateId >= kJavaToBedrock.size()) {
        throw BlockMappingError("stratum-mapping: Java block state id " +
                                std::to_string(javaStateId) + " is out of range (there are " +
                                std::to_string(kJavaToBedrock.size()) + ")");
    }
    const std::uint16_t row = kJavaToBedrock.at(javaStateId);
    BedrockBlockState result{.name = kStrings.at(kBedrockStateNames.at(row)),
                             .states = {},
                             .version = kBedrockBlockStateVersion};
    const std::uint16_t first = kBedrockStateValueOffsets.at(row);
    const std::uint16_t last = kBedrockStateValueOffsets.at(row + 1U);
    result.states.reserve(last - first);
    for (std::uint16_t index = first; index < last; ++index) {
        const BedrockValueRow& value = kBedrockValues.at(kBedrockStateValues.at(index));
        result.states.push_back({.name = kStrings.at(value.key),
                                 .type = value.type,
                                 .integer = value.integer,
                                 .string = value.type == BedrockTagType::String
                                               ? kStrings.at(value.string)
                                               : std::string_view{}});
    }
    return result;
}

BedrockBlockState bedrockBlockState(const settings::BlockState& state) {
    return bedrockBlockState(javaBlockStateId(state));
}

std::size_t javaBlockStateCount() noexcept {
    return kJavaToBedrock.size();
}

std::int32_t bedrockBlockStateVersion() noexcept {
    return kBedrockBlockStateVersion;
}

} // namespace stratum::mapping
