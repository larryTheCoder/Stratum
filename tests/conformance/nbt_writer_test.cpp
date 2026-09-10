// Stratum — NBT writer, against a real chunk.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tests/unit/nbt_writer_test.cpp covers every tag type by hand; this is the
// strongest case available — round-tripping a deeply nested document another
// implementation actually wrote, not one built by hand to suit the writer.
// If write() and read() disagree on anything this format actually uses, this
// is where it would show. The fixture is Mojang-derived and never committed
// (SPEC §12).

#include <stratum/nbt/reader.hpp>
#include <stratum/nbt/writer.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

} // namespace

TEST_CASE("a real chunk's NBT round-trips through write and read unchanged", "[conformance][nbt]") {
    const auto region = fixtures() / "probes" / "no-aquifer" / "seed--1" / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(region)) {
        SKIP("no aquifer-free probe at " << region << "; generate it with "
                                         << "tools/analysis/aquifer-free-probe.sh --accept-eula");
    }
    const auto file = stratum::region::RegionFile::open(region);
    REQUIRE(file.hasChunk(0, 0));
    const auto original = file.readChunk(0, 0);
    const auto originalDocument = stratum::nbt::read(original);

    const auto rewritten = stratum::nbt::write(originalDocument.rootName, originalDocument.root);
    const auto rewrittenDocument = stratum::nbt::read(rewritten);

    CHECK(rewrittenDocument.rootName == originalDocument.rootName);
    CHECK(rewrittenDocument.root == originalDocument.root);
}
