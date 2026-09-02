// Stratum — NBT tag model.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A read-only document model for Minecraft's NBT, as documented on
// minecraft.wiki. No Mojang code was consulted.
//
// Compounds keep their entries in document order rather than sorting them:
// the conformance harness reports where two chunks first differ, and "the
// order vanilla wrote it in" is the useful order to report.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace stratum::nbt {

enum class TagType : std::uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

/// Human-readable name of a tag type, for error messages.
[[nodiscard]] std::string_view tagTypeName(TagType type) noexcept;

/// True for a byte that names a tag type we know.
[[nodiscard]] bool isKnownTagType(std::uint8_t id) noexcept;

class Tag;

/// One entry of a compound, in the order it appeared.
struct NamedTag;

/// Raised when a tag is accessed as the wrong type.
class TypeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Tag {
public:
    using ByteArray = std::vector<std::int8_t>;
    using IntArray = std::vector<std::int32_t>;
    using LongArray = std::vector<std::int64_t>;

    /// A list is homogeneous and remembers its declared element type, which
    /// stays meaningful even when the list is empty.
    struct List {
        TagType elementType = TagType::End;
        std::vector<Tag> elements;

        // Defaulted in-class rather than as a free function after Tag: a
        // free operator== declared later is not visible when Tag's own
        // defaulted operator== is declared, so Clang deletes it while GCC
        // accepts it.
        [[nodiscard]] bool operator==(const List& other) const = default;
    };

    using Compound = std::vector<NamedTag>;

    // Declared here, defined below: see the note above the definitions.
    // The default constructor is an exception: it touches only the variant's
    // first alternative (monostate), so it needs nothing complete.
    Tag() noexcept = default;
    explicit Tag(std::int8_t value) noexcept;
    explicit Tag(std::int16_t value) noexcept;
    explicit Tag(std::int32_t value) noexcept;
    explicit Tag(std::int64_t value) noexcept;
    explicit Tag(float value) noexcept;
    explicit Tag(double value) noexcept;
    explicit Tag(ByteArray value);
    explicit Tag(std::string value);
    explicit Tag(List value);
    explicit Tag(Compound value);
    explicit Tag(IntArray value);
    explicit Tag(LongArray value);

    [[nodiscard]] TagType type() const noexcept;

    [[nodiscard]] bool isEnd() const noexcept { return type() == TagType::End; }

    // Typed access. Each throws TypeError naming both types on a mismatch,
    // because a diff that quietly read a Long as zero would report agreement
    // it never established.
    [[nodiscard]] std::int8_t asByte() const;
    [[nodiscard]] std::int16_t asShort() const;
    [[nodiscard]] std::int32_t asInt() const;
    [[nodiscard]] std::int64_t asLong() const;
    [[nodiscard]] float asFloat() const;
    [[nodiscard]] double asDouble() const;
    [[nodiscard]] const ByteArray& asByteArray() const;
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] const List& asList() const;
    [[nodiscard]] const Compound& asCompound() const;
    [[nodiscard]] const IntArray& asIntArray() const;
    [[nodiscard]] const LongArray& asLongArray() const;

    /// Compound lookup. Returns nullptr when the entry is absent; throws
    /// TypeError if this tag is not a compound.
    [[nodiscard]] const Tag* find(std::string_view name) const;

    /// Compound lookup that throws TypeError naming the missing entry.
    [[nodiscard]] const Tag& at(std::string_view name) const;

    [[nodiscard]] bool operator==(const Tag& other) const;

private:
    std::variant<std::monostate, std::int8_t, std::int16_t, std::int32_t, std::int64_t, float,
                 double, ByteArray, std::string, List, Compound, IntArray, LongArray>
        value_;
};

struct NamedTag {
    std::string name;
    Tag value;

    [[nodiscard]] bool operator==(const NamedTag& other) const = default;
};

// Tag's constructors are defined here rather than in the class body because
// std::variant instantiates its alternatives' special members, and
// std::vector<NamedTag> cannot do that while NamedTag is still incomplete.
// Defining them in-class instantiates the variant too early: GCC tolerates
// it, Clang rejects it outright. Deferring the definitions until NamedTag is
// complete is correct for both.

inline Tag::Tag(std::int8_t value) noexcept : value_(value) {}

inline Tag::Tag(std::int16_t value) noexcept : value_(value) {}

inline Tag::Tag(std::int32_t value) noexcept : value_(value) {}

inline Tag::Tag(std::int64_t value) noexcept : value_(value) {}

inline Tag::Tag(float value) noexcept : value_(value) {}

inline Tag::Tag(double value) noexcept : value_(value) {}

inline Tag::Tag(ByteArray value) : value_(std::move(value)) {}

inline Tag::Tag(std::string value) : value_(std::move(value)) {}

inline Tag::Tag(List value) : value_(std::move(value)) {}

inline Tag::Tag(Compound value) : value_(std::move(value)) {}

inline Tag::Tag(IntArray value) : value_(std::move(value)) {}

inline Tag::Tag(LongArray value) : value_(std::move(value)) {}

inline bool Tag::operator==(const Tag& other) const {
    return value_ == other.value_;
}

} // namespace stratum::nbt
