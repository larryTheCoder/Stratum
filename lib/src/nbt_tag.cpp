// Stratum — NBT tag model.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/nbt/tag.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>

namespace stratum::nbt {

namespace {

/// Variant alternatives are declared in tag-id order, so the index is the id.
[[nodiscard]] TagType typeOfIndex(std::size_t index) noexcept {
    return static_cast<TagType>(static_cast<std::uint8_t>(index));
}

[[noreturn]] void wrongType(TagType actual, TagType wanted) {
    throw TypeError(std::string("NBT tag is ") + std::string(tagTypeName(actual)) + ", not " +
                    std::string(tagTypeName(wanted)));
}

} // namespace

std::string_view tagTypeName(TagType type) noexcept {
    switch (type) {
        case TagType::End:
            return "TAG_End";
        case TagType::Byte:
            return "TAG_Byte";
        case TagType::Short:
            return "TAG_Short";
        case TagType::Int:
            return "TAG_Int";
        case TagType::Long:
            return "TAG_Long";
        case TagType::Float:
            return "TAG_Float";
        case TagType::Double:
            return "TAG_Double";
        case TagType::ByteArray:
            return "TAG_Byte_Array";
        case TagType::String:
            return "TAG_String";
        case TagType::List:
            return "TAG_List";
        case TagType::Compound:
            return "TAG_Compound";
        case TagType::IntArray:
            return "TAG_Int_Array";
        case TagType::LongArray:
            return "TAG_Long_Array";
    }
    return "TAG_Unknown";
}

bool isKnownTagType(std::uint8_t id) noexcept {
    return id <= static_cast<std::uint8_t>(TagType::LongArray);
}

TagType Tag::type() const noexcept {
    return typeOfIndex(value_.index());
}

std::int8_t Tag::asByte() const {
    if (const auto* value = std::get_if<std::int8_t>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Byte);
}

std::int16_t Tag::asShort() const {
    if (const auto* value = std::get_if<std::int16_t>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Short);
}

std::int32_t Tag::asInt() const {
    if (const auto* value = std::get_if<std::int32_t>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Int);
}

std::int64_t Tag::asLong() const {
    if (const auto* value = std::get_if<std::int64_t>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Long);
}

float Tag::asFloat() const {
    if (const auto* value = std::get_if<float>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Float);
}

double Tag::asDouble() const {
    if (const auto* value = std::get_if<double>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Double);
}

const Tag::ByteArray& Tag::asByteArray() const {
    if (const auto* value = std::get_if<ByteArray>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::ByteArray);
}

const std::string& Tag::asString() const {
    if (const auto* value = std::get_if<std::string>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::String);
}

const Tag::List& Tag::asList() const {
    if (const auto* value = std::get_if<List>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::List);
}

const Tag::Compound& Tag::asCompound() const {
    if (const auto* value = std::get_if<Compound>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::Compound);
}

const Tag::IntArray& Tag::asIntArray() const {
    if (const auto* value = std::get_if<IntArray>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::IntArray);
}

const Tag::LongArray& Tag::asLongArray() const {
    if (const auto* value = std::get_if<LongArray>(&value_)) {
        return *value;
    }
    wrongType(type(), TagType::LongArray);
}

const Tag* Tag::find(std::string_view name) const {
    const Compound& entries = asCompound();
    const auto found =
        std::ranges::find_if(entries, [name](const NamedTag& entry) { return entry.name == name; });
    return found == entries.end() ? nullptr : &found->value;
}

const Tag& Tag::at(std::string_view name) const {
    const Tag* found = find(name);
    if (found == nullptr) {
        throw TypeError("NBT compound has no entry named '" + std::string(name) + "'");
    }
    return *found;
}

} // namespace stratum::nbt
