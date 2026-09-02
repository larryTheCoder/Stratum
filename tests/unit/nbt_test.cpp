// Stratum — NBT reader tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Documents are assembled byte by byte here, so each test states exactly
// what the reader is being handed. The malformed cases matter as much as the
// well-formed ones: this parser reads files another implementation wrote,
// and a lenient parser would let the conformance harness report parity it
// never established (SPEC §8).

#include <stratum/nbt/reader.hpp>
#include <stratum/nbt/tag.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using stratum::nbt::Document;
using stratum::nbt::ParseError;
using stratum::nbt::Tag;
using stratum::nbt::TagType;
using stratum::nbt::TypeError;

/// Builds NBT byte streams, big-endian, exactly as the format specifies.
class NbtWriter {
public:
    NbtWriter& u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
        return *this;
    }

    NbtWriter& type(TagType value) { return u8(static_cast<std::uint8_t>(value)); }

    NbtWriter& u16(std::uint16_t value) {
        return u8(static_cast<std::uint8_t>(value >> 8U)).u8(static_cast<std::uint8_t>(value));
    }

    NbtWriter& u32(std::uint32_t value) {
        return u8(static_cast<std::uint8_t>(value >> 24U))
            .u8(static_cast<std::uint8_t>(value >> 16U))
            .u8(static_cast<std::uint8_t>(value >> 8U))
            .u8(static_cast<std::uint8_t>(value));
    }

    NbtWriter& u64(std::uint64_t value) {
        return u32(static_cast<std::uint32_t>(value >> 32U)).u32(static_cast<std::uint32_t>(value));
    }

    NbtWriter& str(std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));
        for (const char character : value) {
            u8(static_cast<std::uint8_t>(character));
        }
        return *this;
    }

    /// A named tag header: type byte followed by the name.
    NbtWriter& named(TagType value, std::string_view name) { return type(value).str(name); }

    NbtWriter& end() { return type(TagType::End); }

    [[nodiscard]] std::span<const std::byte> span() const noexcept { return bytes_; }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

/// The canonical example document from the format's documentation.
[[nodiscard]] NbtWriter helloWorld() {
    NbtWriter writer;
    writer.named(TagType::Compound, "hello world");
    writer.named(TagType::String, "name").str("Bananrama");
    writer.end();
    return writer;
}

} // namespace

TEST_CASE("the canonical hello world document parses", "[nbt]") {
    const NbtWriter writer = helloWorld();
    const Document document = stratum::nbt::read(writer.span());

    CHECK(document.rootName == "hello world");
    CHECK(document.bytesConsumed == writer.bytes().size());
    REQUIRE(document.root.type() == TagType::Compound);
    REQUIRE(document.root.asCompound().size() == 1U);
    CHECK(document.root.at("name").asString() == "Bananrama");
}

TEST_CASE("every tag type round-trips", "[nbt]") {
    // Floats and doubles are written as bit patterns and compared as bit
    // patterns: NBT parity is exact, and -0.0 must not compare equal to 0.0.
    constexpr std::uint32_t kFloatBits = 0xC0490FDBU;            // -3.14159274f
    constexpr std::uint64_t kDoubleBits = 0x8000000000000000ULL; // -0.0

    NbtWriter writer;
    writer.named(TagType::Compound, "root");
    writer.named(TagType::Byte, "byte").u8(0x80U);      // -128
    writer.named(TagType::Short, "short").u16(0x8000U); // -32768
    writer.named(TagType::Int, "int").u32(0x80000000U); // INT_MIN
    writer.named(TagType::Long, "long").u64(0x8000000000000000ULL);
    writer.named(TagType::Float, "float").u32(kFloatBits);
    writer.named(TagType::Double, "double").u64(kDoubleBits);
    writer.named(TagType::ByteArray, "bytes").u32(3).u8(1).u8(0xFFU).u8(0x7FU);
    writer.named(TagType::String, "string").str("minecraft:stone");
    writer.named(TagType::IntArray, "ints").u32(2).u32(0xFFFFFFFFU).u32(7);
    writer.named(TagType::LongArray, "longs").u32(1).u64(0xFFFFFFFFFFFFFFFFULL);
    writer.named(TagType::List, "list").type(TagType::Int).u32(3).u32(10).u32(20).u32(30);
    writer.named(TagType::List, "empty").type(TagType::End).u32(0);
    writer.named(TagType::Compound, "nested");
    writer.named(TagType::Byte, "inner").u8(42);
    writer.end();
    writer.end();

    const Document document = stratum::nbt::read(writer.span());
    const Tag& root = document.root;

    CHECK(root.at("byte").asByte() == -128);
    CHECK(root.at("short").asShort() == -32768);
    CHECK(root.at("int").asInt() == -2147483647 - 1);
    CHECK(root.at("long").asLong() == INT64_MIN);
    CHECK(std::bit_cast<std::uint32_t>(root.at("float").asFloat()) == kFloatBits);
    CHECK(std::bit_cast<std::uint64_t>(root.at("double").asDouble()) == kDoubleBits);
    CHECK(root.at("bytes").asByteArray() == Tag::ByteArray{1, -1, 127});
    CHECK(root.at("string").asString() == "minecraft:stone");
    CHECK(root.at("ints").asIntArray() == Tag::IntArray{-1, 7});
    CHECK(root.at("longs").asLongArray() == Tag::LongArray{-1});

    const Tag::List& list = root.at("list").asList();
    CHECK(list.elementType == TagType::Int);
    REQUIRE(list.elements.size() == 3U);
    CHECK(list.elements[2].asInt() == 30);

    const Tag::List& empty = root.at("empty").asList();
    CHECK(empty.elementType == TagType::End);
    CHECK(empty.elements.empty());

    CHECK(root.at("nested").at("inner").asByte() == 42);
    CHECK(document.bytesConsumed == writer.bytes().size());
}

TEST_CASE("compound entries keep the order they were written in", "[nbt]") {
    // The harness reports where two chunks first differ; vanilla's own
    // ordering is the useful one to report it in.
    NbtWriter writer;
    writer.named(TagType::Compound, "");
    writer.named(TagType::Byte, "zebra").u8(1);
    writer.named(TagType::Byte, "aardvark").u8(2);
    writer.named(TagType::Byte, "mongoose").u8(3);
    writer.end();

    const Document document = stratum::nbt::read(writer.span());
    const Tag::Compound& entries = document.root.asCompound();
    REQUIRE(entries.size() == 3U);
    CHECK(entries[0].name == "zebra");
    CHECK(entries[1].name == "aardvark");
    CHECK(entries[2].name == "mongoose");
}

TEST_CASE("lists of compounds nest, as chunk sections do", "[nbt]") {
    // The shape the harness actually walks: sections -> block_states ->
    // palette plus a packed long array.
    NbtWriter writer;
    writer.named(TagType::Compound, "");
    writer.named(TagType::List, "sections").type(TagType::Compound).u32(2);
    for (std::uint8_t y : {std::uint8_t{0}, std::uint8_t{1}}) {
        writer.named(TagType::Byte, "Y").u8(y);
        writer.named(TagType::Compound, "block_states");
        writer.named(TagType::List, "palette").type(TagType::Compound).u32(1);
        writer.named(TagType::String, "Name").str("minecraft:stone");
        writer.end();
        writer.named(TagType::LongArray, "data").u32(1).u64(0x0123456789ABCDEFULL);
        writer.end();
        writer.end();
    }
    writer.end();

    const Document document = stratum::nbt::read(writer.span());
    const Tag::List& sections = document.root.at("sections").asList();
    REQUIRE(sections.elements.size() == 2U);
    CHECK(sections.elements[1].at("Y").asByte() == 1);
    const Tag& states = sections.elements[0].at("block_states");
    CHECK(states.at("palette").asList().elements[0].at("Name").asString() == "minecraft:stone");
    CHECK(states.at("data").asLongArray()[0] == INT64_C(0x0123456789ABCDEF));
}

TEST_CASE("accessing a tag as the wrong type throws, naming both", "[nbt]") {
    const Document document = stratum::nbt::read(helloWorld().span());
    REQUIRE_THROWS_AS(document.root.at("name").asInt(), TypeError);
    REQUIRE_THROWS_WITH(document.root.at("name").asInt(),
                        Catch::Matchers::ContainsSubstring("TAG_String") &&
                            Catch::Matchers::ContainsSubstring("TAG_Int"));
    REQUIRE_THROWS_WITH(document.root.at("absent"), Catch::Matchers::ContainsSubstring("absent"));
    CHECK(document.root.find("absent") == nullptr);
    CHECK(document.root.find("name") != nullptr);
}

TEST_CASE("truncation is caught wherever it happens", "[nbt][malformed]") {
    // Every prefix of a valid document is invalid, and none of them may be
    // read as a partial success.
    const NbtWriter complete = helloWorld();
    const std::vector<std::byte>& bytes = complete.bytes();

    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        const std::span<const std::byte> truncated(bytes.data(), length);
        CHECK_THROWS_AS(stratum::nbt::read(truncated), ParseError);
    }
    CHECK_NOTHROW(stratum::nbt::read(complete.span()));
}

TEST_CASE("malformed structure is rejected with the byte offset", "[nbt][malformed]") {
    SECTION("an unknown tag type") {
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(static_cast<TagType>(99), "bad");
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("unknown tag type 99"));
    }

    SECTION("a root that is not a compound") {
        NbtWriter writer;
        writer.named(TagType::Int, "").u32(1);
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("TAG_Compound"));
    }

    SECTION("a negative array length") {
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(TagType::IntArray, "ints").u32(0xFFFFFFFFU);
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("negative length"));
    }

    SECTION("a length that cannot possibly fit") {
        // The allocation this would imply must never be attempted.
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(TagType::LongArray, "longs").u32(0x7FFFFFFFU);
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("but only"));
    }

    SECTION("a list of TAG_End claiming elements") {
        // TAG_End has no payload, so a count that clears the generic
        // "could this many elements fit?" check would still materialise that
        // many tags out of nothing. Hence the dedicated rule.
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(TagType::List, "list").type(TagType::End).u32(1);
        writer.end();
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("TAG_End"));
    }

    SECTION("a list whose element count cannot fit in the remaining bytes") {
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(TagType::List, "list").type(TagType::Compound).u32(0x7FFFFFFFU);
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("but only"));
    }

    SECTION("duplicate compound keys") {
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(TagType::Byte, "same").u8(1);
        writer.named(TagType::Byte, "same").u8(2);
        writer.end();
        REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span()),
                            Catch::Matchers::ContainsSubstring("duplicate"));
    }

    SECTION("a compound that never ends") {
        NbtWriter writer;
        writer.named(TagType::Compound, "");
        writer.named(TagType::Byte, "value").u8(1);
        CHECK_THROWS_AS(stratum::nbt::read(writer.span()), ParseError);
    }
}

TEST_CASE("nesting deeper than the limit is refused", "[nbt][malformed]") {
    // A corrupt file must not be able to exhaust the stack.
    constexpr std::size_t kDepth = 200;
    NbtWriter writer;
    writer.named(TagType::Compound, "");
    for (std::size_t i = 0; i < kDepth; ++i) {
        writer.named(TagType::Compound, "n");
    }
    for (std::size_t i = 0; i <= kDepth; ++i) {
        writer.end();
    }

    CHECK_NOTHROW(stratum::nbt::read(writer.span()));

    stratum::nbt::ReadLimits shallow;
    shallow.maxDepth = 8;
    REQUIRE_THROWS_WITH(stratum::nbt::read(writer.span(), shallow),
                        Catch::Matchers::ContainsSubstring("nesting deeper"));
}

TEST_CASE("trailing bytes are reported rather than assumed away", "[nbt]") {
    NbtWriter writer = helloWorld();
    const std::size_t documentSize = writer.bytes().size();
    writer.u8(0xAAU).u8(0xBBU);

    const Document document = stratum::nbt::read(writer.span());
    CHECK(document.bytesConsumed == documentSize);
    CHECK(document.bytesConsumed < writer.bytes().size());
}
