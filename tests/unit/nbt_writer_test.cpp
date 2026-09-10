// Stratum — NBT writer tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The writer's own claim is round-trip fidelity against the reader: nothing
// here re-derives NBT's layout by hand (that is what tests/support/
// nbt_writer.hpp is for, on the reader side) — every case here builds a Tag
// tree, writes it, reads it back, and checks the result equals what went in.
// The strongest case — round-tripping a real chunk another implementation
// wrote — needs the (Mojang-derived, never committed) fixtures and lives in
// tests/conformance/nbt_writer_test.cpp instead, same split as everywhere
// else in this project.

#include <stratum/nbt/reader.hpp>
#include <stratum/nbt/tag.hpp>
#include <stratum/nbt/writer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using stratum::nbt::NamedTag;
using stratum::nbt::Tag;
using stratum::nbt::TagType;
using stratum::nbt::WriteError;

TEST_CASE("every tag type round-trips through write and read", "[nbt][writer]") {
    Tag::Compound entries;
    entries.push_back(NamedTag{.name = "a byte", .value = Tag{std::int8_t{-12}}});
    entries.push_back(NamedTag{.name = "a short", .value = Tag{std::int16_t{-1234}}});
    entries.push_back(NamedTag{.name = "an int", .value = Tag{std::int32_t{-123456}}});
    entries.push_back(NamedTag{.name = "a long", .value = Tag{std::int64_t{-123456789012LL}}});
    entries.push_back(NamedTag{.name = "a float", .value = Tag{1.5F}});
    entries.push_back(NamedTag{.name = "a double", .value = Tag{-0.0}}); // exact bits, not 0.0
    entries.push_back(
        NamedTag{.name = "a byte array", .value = Tag{Tag::ByteArray{1, -2, 3, -128, 127}}});
    entries.push_back(NamedTag{.name = "a string", .value = Tag{std::string{"hello world"}}});
    entries.push_back(NamedTag{.name = "an empty string", .value = Tag{std::string{}}});
    entries.push_back(NamedTag{.name = "an int array", .value = Tag{Tag::IntArray{1, -2, 3}}});
    entries.push_back(NamedTag{.name = "a long array", .value = Tag{Tag::LongArray{1, -2, 3, 0}}});
    entries.push_back(NamedTag{
        .name = "a list of strings",
        .value = Tag{Tag::List{.elementType = TagType::String,
                               .elements = {Tag{std::string{"a"}}, Tag{std::string{"b"}}}}}});
    entries.push_back(NamedTag{.name = "an empty list", .value = Tag{Tag::List{}}});
    entries.push_back(NamedTag{
        .name = "a nested compound",
        .value = Tag{Tag::Compound{NamedTag{.name = "inner", .value = Tag{std::int32_t{7}}}}}});

    const Tag root{entries};
    const auto bytes = stratum::nbt::write("root name", root);
    const auto document = stratum::nbt::read(bytes);

    CHECK(document.rootName == "root name");
    CHECK(document.bytesConsumed == bytes.size());
    CHECK(document.root == root);
}

TEST_CASE("writing a non-compound root is refused", "[nbt][writer]") {
    CHECK_THROWS_AS(stratum::nbt::write("name", Tag{std::int32_t{1}}), WriteError);
    CHECK_THROWS_WITH(stratum::nbt::write("name", Tag{std::int32_t{1}}),
                      ContainsSubstring("TAG_Compound"));
}
