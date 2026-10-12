// Stratum — a probe corpus's spec.json, and the one way a case looks in it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/analysis/density-probe.sh copies the spec a corpus was generated
// from into the corpus as spec.json: an array of entries, one per dimension,
// each named. It is the only record of what the server was given, so a case
// reads its dimensions' recipes from there and nowhere else
// (support/probe_settings.hpp turns an entry into its noise settings).
//
// Every case once carried its own lookup, and some of them FAILed and then
// returned, which MSVC rejects as unreachable code (C4702; lint rule 9 now
// catches the shape). These REQUIRE instead, naming the file and the
// dimension, so a corpus without the entry a case scores fails the case
// rather than letting it score something else (SPEC §8).
#pragma once

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace stratum::test {

/// The spec.json of the probe corpus in @p dir. Fails the running case,
/// naming the file, unless it is there and holds an array of entries.
[[nodiscard]] inline nlohmann::json readSpec(const std::filesystem::path& dir) {
    const std::filesystem::path file = dir / "spec.json";
    INFO(file << " is missing or is not a probe spec; regenerate the corpus");
    std::ifstream in(file);
    REQUIRE(in.good());
    nlohmann::json spec = nlohmann::json::parse(in, nullptr, false);
    REQUIRE(!spec.is_discarded());
    REQUIRE(spec.is_array());
    return spec;
}

/// The entry named @p name in @p spec, the spec.json of the corpus in @p dir
/// (which only names it in a failure). Fails the running case unless exactly
/// one entry carries that name.
[[nodiscard]] inline const nlohmann::json&
specEntry(const nlohmann::json& spec, const std::filesystem::path& dir, std::string_view name) {
    const nlohmann::json* named = nullptr;
    std::size_t matches = 0;
    for (const auto& entry : spec) {
        if (entry.is_object() && entry.contains("name") && entry.at("name").is_string() &&
            entry.at("name").get_ref<const std::string&>() == name) {
            named = &entry;
            ++matches;
        }
    }
    INFO((dir / "spec.json") << " has " << matches << " entries named " << name
                             << ", where the case reads exactly one");
    REQUIRE(matches == 1);
    return *named;
}

/// The entry named @p name in the spec.json of the corpus in @p dir: readSpec,
/// then specEntry, with the same failures.
[[nodiscard]] inline nlohmann::json specEntry(const std::filesystem::path& dir,
                                              std::string_view name) {
    const nlohmann::json spec = readSpec(dir);
    return specEntry(spec, dir, name);
}

} // namespace stratum::test
