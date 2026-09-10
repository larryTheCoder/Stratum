// Stratum — NBT writer.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/nbt/writer.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stratum::nbt {

namespace {

void appendByte(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void appendUint16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value >> 8U));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}

void appendUint32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>(value >> static_cast<unsigned>(shift)));
    }
}

void appendUint64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>(value >> static_cast<unsigned>(shift)));
    }
}

/// Length-prefixed modified UTF-8. Written verbatim, matching how the reader
/// keeps it — see reader.hpp's own note on why re-encoding would be wrong.
void appendString(std::vector<std::byte>& out, const std::string& value) {
    appendUint16(out, static_cast<std::uint16_t>(value.size()));
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
}

void writeTagType(std::vector<std::byte>& out, TagType type) {
    appendByte(out, static_cast<std::uint8_t>(type));
}

void writePayload(std::vector<std::byte>& out, const Tag& tag);

void writeCompound(std::vector<std::byte>& out, const Tag::Compound& entries) {
    for (const NamedTag& entry : entries) {
        writeTagType(out, entry.value.type());
        appendString(out, entry.name);
        writePayload(out, entry.value);
    }
    writeTagType(out, TagType::End);
}

void writeList(std::vector<std::byte>& out, const Tag::List& list) {
    writeTagType(out, list.elementType);
    appendUint32(out, static_cast<std::uint32_t>(list.elements.size()));
    for (const Tag& element : list.elements) {
        writePayload(out, element);
    }
}

void writePayload(std::vector<std::byte>& out, const Tag& tag) {
    switch (tag.type()) {
        case TagType::End:
            return;
        case TagType::Byte:
            appendByte(out, static_cast<std::uint8_t>(tag.asByte()));
            return;
        case TagType::Short:
            appendUint16(out, static_cast<std::uint16_t>(tag.asShort()));
            return;
        case TagType::Int:
            appendUint32(out, static_cast<std::uint32_t>(tag.asInt()));
            return;
        case TagType::Long:
            appendUint64(out, static_cast<std::uint64_t>(tag.asLong()));
            return;
        case TagType::Float:
            appendUint32(out, std::bit_cast<std::uint32_t>(tag.asFloat()));
            return;
        case TagType::Double:
            appendUint64(out, std::bit_cast<std::uint64_t>(tag.asDouble()));
            return;
        case TagType::ByteArray: {
            const Tag::ByteArray& values = tag.asByteArray();
            appendUint32(out, static_cast<std::uint32_t>(values.size()));
            for (const std::int8_t value : values) {
                appendByte(out, static_cast<std::uint8_t>(value));
            }
            return;
        }
        case TagType::String:
            appendString(out, tag.asString());
            return;
        case TagType::List:
            writeList(out, tag.asList());
            return;
        case TagType::Compound:
            writeCompound(out, tag.asCompound());
            return;
        case TagType::IntArray: {
            const Tag::IntArray& values = tag.asIntArray();
            appendUint32(out, static_cast<std::uint32_t>(values.size()));
            for (const std::int32_t value : values) {
                appendUint32(out, static_cast<std::uint32_t>(value));
            }
            return;
        }
        case TagType::LongArray: {
            const Tag::LongArray& values = tag.asLongArray();
            appendUint32(out, static_cast<std::uint32_t>(values.size()));
            for (const std::int64_t value : values) {
                appendUint64(out, static_cast<std::uint64_t>(value));
            }
            return;
        }
    }
}

} // namespace

std::vector<std::byte> write(const std::string& rootName, const Tag& root) {
    if (root.type() != TagType::Compound) {
        throw WriteError("a document's root tag must be TAG_Compound, not " +
                         std::string(tagTypeName(root.type())));
    }
    std::vector<std::byte> out;
    writeTagType(out, TagType::Compound);
    appendString(out, rootName);
    writeCompound(out, root.asCompound());
    return out;
}

} // namespace stratum::nbt
