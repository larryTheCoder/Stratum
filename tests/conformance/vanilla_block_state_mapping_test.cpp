// Stratum — every vanilla block state resolves to a Bedrock blockstate.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/mapping-sync already refuses to generate a table that disagrees
// with vanilla's block report. This makes the same checks again through the
// compiled loader instead of the generator's Python — so a bug in how the
// C++ side numbers states or reads the table fails a test too — and adds the
// one that matters most to generation: every block state vanilla's own
// noise settings can emit resolves.
#include <stratum/mapping/block_state.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using stratum::data::ResourceLocation;
using stratum::mapping::bedrockBlockState;
using stratum::mapping::javaBlockStateCount;
using stratum::mapping::javaBlockStateId;
using stratum::settings::BlockState;

namespace {

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream file(path);
    REQUIRE(file);
    return nlohmann::json::parse(file);
}

[[nodiscard]] BlockState blockState(const std::string& name, const nlohmann::json& properties) {
    BlockState state{.name = ResourceLocation::parse(name), .properties = {}};
    for (const auto& [key, value] : properties.items()) {
        state.properties.emplace(key, value.get<std::string>());
    }
    return state;
}

void collectBlockStates(const nlohmann::json& node, std::set<std::uint32_t>& ids) {
    if (node.is_object()) {
        if (const auto name = node.find("Name"); name != node.end() && name->is_string()) {
            ids.insert(javaBlockStateId(blockState(
                name->get<std::string>(), node.value("Properties", nlohmann::json::object()))));
        }
        for (const auto& child : node) {
            collectBlockStates(child, ids);
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            collectBlockStates(child, ids);
        }
    }
}

} // namespace

TEST_CASE("every block state in vanilla's report has its own id and a Bedrock state", "[mapping]") {
    const std::filesystem::path report = versionDir() / "reports" / "blocks.json";
    if (!std::filesystem::is_regular_file(report)) {
        SKIP("no reports/blocks.json under " << STRATUM_FIXTURES_DIR
                                             << "; run tools/fetch-vanilla first");
    }

    std::size_t checked = 0;
    const nlohmann::json blocks = readJson(report);
    for (const auto& [name, block] : blocks.items()) {
        for (const auto& state : block.at("states")) {
            const BlockState java =
                blockState(name, state.value("properties", nlohmann::json::object()));
            const auto id = state.at("id").get<std::uint32_t>();
            INFO("block state " << name << " "
                                << state.value("properties", nlohmann::json::object()).dump());
            CHECK(javaBlockStateId(java) == id);
            CHECK_FALSE(bedrockBlockState(id).name.empty());
            if (state.value("default", false)) {
                CHECK(javaBlockStateId(blockState(name, nlohmann::json::object())) == id);
            }
            ++checked;
        }
    }
    CHECK(checked == javaBlockStateCount());
}

TEST_CASE("every block state vanilla's noise settings can emit resolves", "[mapping]") {
    const std::filesystem::path settingsDir = versionDir() / "worldgen" / "noise_settings";
    if (!std::filesystem::is_directory(settingsDir)) {
        SKIP("no worldgen/noise_settings fixtures under " << STRATUM_FIXTURES_DIR
                                                          << "; run tools/fetch-vanilla first");
    }

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(settingsDir)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        INFO("noise settings " << entry.path().filename().string());
        std::set<std::uint32_t> ids;
        REQUIRE_NOTHROW(collectBlockStates(readJson(entry.path()), ids));
        CHECK_FALSE(ids.empty());
        for (const std::uint32_t id : ids) {
            CHECK_FALSE(bedrockBlockState(id).name.empty());
        }
        ++files;
    }
    CHECK(files > 0);
}
