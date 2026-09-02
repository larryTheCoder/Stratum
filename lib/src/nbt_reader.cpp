// Stratum — NBT reader.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/nbt/reader.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace stratum::nbt {

namespace {

/// A bounds-checked cursor over the input. Every read reports the offset it
/// failed at, because "malformed NBT" on its own is not actionable when the
/// input is a 200 KiB chunk.
class Cursor {
public:
    explicit Cursor(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

    [[noreturn]] void fail(const std::string& what) const {
        throw ParseError("NBT at byte " + std::to_string(offset_) + ": " + what);
    }

    void require(std::size_t count, const std::string& what) const {
        if (remaining() < count) {
            fail("needs " + std::to_string(count) + " byte(s) for " + what + " but only " +
                 std::to_string(remaining()) + " remain");
        }
    }

    [[nodiscard]] std::uint8_t readByte(const std::string& what) {
        require(1, what);
        return static_cast<std::uint8_t>(bytes_[offset_++]);
    }

    [[nodiscard]] std::uint16_t readUint16(const std::string& what) {
        require(2, what);
        const auto high = static_cast<std::uint16_t>(bytes_[offset_]);
        const auto low = static_cast<std::uint16_t>(bytes_[offset_ + 1]);
        offset_ += 2;
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(high << 8U) | low);
    }

    [[nodiscard]] std::uint32_t readUint32(const std::string& what) {
        require(4, what);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value = (value << 8U) | static_cast<std::uint32_t>(bytes_[offset_ + i]);
        }
        offset_ += 4;
        return value;
    }

    [[nodiscard]] std::uint64_t readUint64(const std::string& what) {
        require(8, what);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value = (value << 8U) | static_cast<std::uint64_t>(bytes_[offset_ + i]);
        }
        offset_ += 8;
        return value;
    }

    /// Length-prefixed modified UTF-8. The bytes are kept verbatim: vanilla
    /// worldgen identifiers are ASCII, where modified UTF-8 and UTF-8 agree,
    /// and re-encoding would make byte-for-byte comparison meaningless.
    [[nodiscard]] std::string readString(const std::string& what) {
        const std::size_t length = readUint16(what + " length");
        require(length, what);
        std::string value(length, '\0');
        if (length > 0) {
            std::memcpy(value.data(), bytes_.data() + offset_, length);
        }
        offset_ += length;
        return value;
    }

    /// Reads an element count and checks it can possibly fit, before any
    /// allocation happens. @p bytesPerElement is 0 for variable-size
    /// elements, which still occupy at least one byte each.
    [[nodiscard]] std::size_t readCount(const std::string& what, std::size_t bytesPerElement) {
        const auto raw = static_cast<std::int32_t>(readUint32(what + " length"));
        if (raw < 0) {
            fail(what + " has a negative length (" + std::to_string(raw) + ")");
        }
        const auto count = static_cast<std::size_t>(raw);
        const std::size_t minimumBytes = count * (bytesPerElement == 0 ? 1U : bytesPerElement);
        if (minimumBytes > remaining()) {
            fail(what + " declares " + std::to_string(count) + " element(s), needing at least " +
                 std::to_string(minimumBytes) + " byte(s), but only " +
                 std::to_string(remaining()) + " remain");
        }
        return count;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

class Reader {
public:
    Reader(Cursor cursor, const ReadLimits& limits) : cursor_(cursor), limits_(limits) {}

    [[nodiscard]] Tag readPayload(TagType type, std::size_t depth) {
        if (depth > limits_.maxDepth) {
            cursor_.fail("nesting deeper than the limit of " + std::to_string(limits_.maxDepth));
        }

        switch (type) {
            case TagType::End:
                return Tag{};
            case TagType::Byte:
                return Tag{static_cast<std::int8_t>(cursor_.readByte("TAG_Byte"))};
            case TagType::Short:
                return Tag{static_cast<std::int16_t>(cursor_.readUint16("TAG_Short"))};
            case TagType::Int:
                return Tag{static_cast<std::int32_t>(cursor_.readUint32("TAG_Int"))};
            case TagType::Long:
                return Tag{static_cast<std::int64_t>(cursor_.readUint64("TAG_Long"))};
            case TagType::Float:
                return Tag{std::bit_cast<float>(cursor_.readUint32("TAG_Float"))};
            case TagType::Double:
                return Tag{std::bit_cast<double>(cursor_.readUint64("TAG_Double"))};
            case TagType::ByteArray:
                return Tag{readByteArray()};
            case TagType::String:
                return Tag{cursor_.readString("TAG_String")};
            case TagType::List:
                return Tag{readList(depth)};
            case TagType::Compound:
                return Tag{readCompound(depth)};
            case TagType::IntArray:
                return Tag{readIntArray()};
            case TagType::LongArray:
                return Tag{readLongArray()};
        }
        cursor_.fail("unreachable tag type");
    }

    [[nodiscard]] TagType readTagType() {
        const std::uint8_t id = cursor_.readByte("tag type");
        if (!isKnownTagType(id)) {
            cursor_.fail("unknown tag type " + std::to_string(id));
        }
        return static_cast<TagType>(id);
    }

    [[nodiscard]] Cursor& cursor() noexcept { return cursor_; }

private:
    [[nodiscard]] Tag::ByteArray readByteArray() {
        const std::size_t count = cursor_.readCount("TAG_Byte_Array", 1);
        Tag::ByteArray values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = static_cast<std::int8_t>(cursor_.readByte("TAG_Byte_Array element"));
        }
        return values;
    }

    [[nodiscard]] Tag::IntArray readIntArray() {
        const std::size_t count = cursor_.readCount("TAG_Int_Array", 4);
        Tag::IntArray values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = static_cast<std::int32_t>(cursor_.readUint32("TAG_Int_Array element"));
        }
        return values;
    }

    [[nodiscard]] Tag::LongArray readLongArray() {
        const std::size_t count = cursor_.readCount("TAG_Long_Array", 8);
        Tag::LongArray values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = static_cast<std::int64_t>(cursor_.readUint64("TAG_Long_Array element"));
        }
        return values;
    }

    [[nodiscard]] Tag::List readList(std::size_t depth) {
        Tag::List list;
        list.elementType = readTagType();
        const std::size_t count = cursor_.readCount("TAG_List", 0);

        if (list.elementType == TagType::End && count != 0) {
            // TAG_End carries no payload, so such a list could claim any
            // length at no cost in bytes. Vanilla writes length 0 here.
            cursor_.fail("TAG_List of TAG_End declares " + std::to_string(count) + " elements");
        }

        list.elements.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            list.elements.push_back(readPayload(list.elementType, depth + 1));
        }
        return list;
    }

    [[nodiscard]] Tag::Compound readCompound(std::size_t depth) {
        Tag::Compound entries;
        while (true) {
            const TagType type = readTagType();
            if (type == TagType::End) {
                return entries;
            }
            std::string name = cursor_.readString("compound entry name");
            for (const NamedTag& existing : entries) {
                if (existing.name == name) {
                    cursor_.fail("compound has a duplicate entry named '" + name + "'");
                }
            }
            Tag value = readPayload(type, depth + 1);
            entries.push_back(NamedTag{std::move(name), std::move(value)});
        }
    }

    Cursor cursor_;
    ReadLimits limits_;
};

} // namespace

Document read(std::span<const std::byte> bytes, const ReadLimits& limits) {
    Reader reader(Cursor(bytes), limits);

    const TagType rootType = reader.readTagType();
    if (rootType != TagType::Compound) {
        reader.cursor().fail("root tag is " + std::string(tagTypeName(rootType)) +
                             ", but a file's root must be TAG_Compound");
    }

    Document document;
    document.rootName = reader.cursor().readString("root name");
    document.root = reader.readPayload(rootType, 0);
    document.bytesConsumed = reader.cursor().offset();
    return document;
}

} // namespace stratum::nbt
