// Stratum — NBT reader against real vanilla files.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite reads documents this repository wrote, which proves the
// reader agrees with itself. This one reads the structure NBT that vanilla
// wrote — the only real, vanilla-produced NBT available without running a
// server — and is the first thing here to check our understanding of the
// format against Mojang's actual output rather than against a reading of the
// documentation.
//
// The files are Mojang-derived and never committed (SPEC §12). Without them
// this suite SKIPs, loudly, naming the command that produces them.

#include <stratum/nbt/reader.hpp>
#include <stratum/nbt/tag.hpp>

#include <catch2/catch_test_macros.hpp>

#define ZLIB_CONST
#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

using stratum::nbt::Document;
using stratum::nbt::Tag;
using stratum::nbt::TagType;

[[nodiscard]] std::vector<std::filesystem::path> findStructureFiles() {
    std::vector<std::filesystem::path> files;
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".nbt") {
            files.push_back(entry.path());
        }
    }
    // Directory iteration order is unspecified; parity work needs a stable
    // order so a failure names the same file on every machine.
    std::sort(files.begin(), files.end());
    return files;
}

[[nodiscard]] std::vector<std::byte> readAll(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    stream.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size);
    REQUIRE(stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)));
    return bytes;
}

/// Structure files are gzip-framed.
[[nodiscard]] std::vector<std::byte> gunzip(const std::vector<std::byte>& input) {
    z_stream stream{};
    REQUIRE(inflateInit2(&stream, 15 + 16) == Z_OK);
    stream.next_in = reinterpret_cast<const Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());

    std::vector<std::byte> output;
    std::vector<std::byte> buffer(64U * 1024U);
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            FAIL("inflate failed with status " << status);
        }
        const std::size_t produced = buffer.size() - static_cast<std::size_t>(stream.avail_out);
        output.insert(output.end(), buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(produced));
    } while (status != Z_STREAM_END);
    inflateEnd(&stream);
    return output;
}

} // namespace

TEST_CASE("every vanilla structure NBT file parses", "[conformance][nbt]") {
    const std::vector<std::filesystem::path> files = findStructureFiles();
    if (files.empty()) {
        SKIP("no vanilla .nbt fixtures under "
             << STRATUM_FIXTURES_DIR
             << " — they are Mojang-derived and never committed (SPEC §12). Generate them "
                "with: tools/fetch-vanilla --with-structures");
    }

    std::size_t parsed = 0;
    std::size_t totalBytes = 0;

    for (const std::filesystem::path& path : files) {
        CAPTURE(path.string());
        const std::vector<std::byte> raw = gunzip(readAll(path));
        totalBytes += raw.size();

        const Document document = stratum::nbt::read(raw);

        // Vanilla writes file NBT with an unnamed root compound, and nothing
        // after it: if either were untrue our framing understanding is wrong.
        CHECK(document.rootName.empty());
        CHECK(document.bytesConsumed == raw.size());
        REQUIRE(document.root.type() == TagType::Compound);

        // The shape vanilla actually writes. Worth stating plainly: `size`
        // and `pos` below are TAG_List of TAG_Int, not TAG_Int_Array, which
        // is what this suite caught the first time it ran against real files.
        const Tag& root = document.root;
        REQUIRE(root.find("size") != nullptr);
        const Tag::List& size = root.at("size").asList();
        CHECK(size.elementType == TagType::Int);
        REQUIRE(size.elements.size() == 3U);
        for (const Tag& axis : size.elements) {
            CHECK(axis.asInt() > 0);
        }
        REQUIRE(root.find("blocks") != nullptr);
        CHECK(root.at("DataVersion").asInt() > 0);

        // Two forms exist in the real data, and only the singular one is the
        // obvious guess: most files carry one `palette`, while randomised
        // variants carry `palettes`, a list of interchangeable palettes.
        // Exactly one file in 1.21.11 uses the plural form, which is the sort
        // of thing only real input tells you.
        std::vector<const Tag::List*> palettes;
        if (const Tag* single = root.find("palette"); single != nullptr) {
            palettes.push_back(&single->asList());
        } else if (const Tag* multiple = root.find("palettes"); multiple != nullptr) {
            for (const Tag& alternative : multiple->asList().elements) {
                palettes.push_back(&alternative.asList());
            }
        }
        REQUIRE_FALSE(palettes.empty());

        std::size_t smallestPalette = std::numeric_limits<std::size_t>::max();
        for (const Tag::List* palette : palettes) {
            CHECK(palette->elementType == TagType::Compound);
            smallestPalette = std::min(smallestPalette, palette->elements.size());
            for (const Tag& entry : palette->elements) {
                // Every palette entry names a block; that name is what the
                // Bedrock mapping layer will later key on.
                CHECK_FALSE(entry.at("Name").asString().empty());
            }
        }

        const Tag::List& blocks = root.at("blocks").asList();
        CHECK(blocks.elementType == TagType::Compound);
        for (const Tag& block : blocks.elements) {
            const Tag::List& pos = block.at("pos").asList();
            CHECK(pos.elementType == TagType::Int);
            REQUIRE(pos.elements.size() == 3U);
            // Positions are relative to the structure origin and must lie
            // inside the declared size.
            for (std::size_t axis = 0; axis < 3U; ++axis) {
                const std::int32_t coordinate = pos.elements[axis].asInt();
                CHECK(coordinate >= 0);
                CHECK(coordinate < size.elements[axis].asInt());
            }
            // A block's state indexes into whichever palette is chosen, so
            // it has to be valid in every alternative.
            const std::int32_t state = block.at("state").asInt();
            CHECK(state >= 0);
            CHECK(static_cast<std::size_t>(state) < smallestPalette);
        }

        ++parsed;
    }

    WARN("parsed " << parsed << " vanilla structure files, " << totalBytes << " bytes of NBT");
    CHECK(parsed == files.size());
}
