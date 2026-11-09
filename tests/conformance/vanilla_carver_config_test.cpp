// Stratum — vanilla's configured carvers, its biomes' carvers fields and its
// carver-replaceable tags, read by this build.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The data side of the carvers (M6): every configured carver the pinned pack
// ships parses; every biome's `carvers` field parses, in the forms the
// shipped data uses (spec/carvers-b-spec.md Q11.1: the Nether's are bare
// ids); and the two replaceable tags resolve to exactly the block lists the
// clean-room spec records (spec/carvers-a-constants.json
// `resolved_replaceables`, spec/carvers-a-spec.md Q12.2: 52 and 25 blocks).

#include <stratum/carver/config.hpp>
#include <stratum/data/block_tags.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

using stratum::carver::CarverEntry;
using stratum::carver::CarverType;
using stratum::carver::ConfiguredCarver;
using stratum::data::BlockTags;
using stratum::data::ResourceLocation;

namespace {

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

} // namespace

TEST_CASE("vanilla's configured carvers parse", "[conformance][carver]") {
    const std::filesystem::path dir = versionDir() / "worldgen" / "configured_carver";
    if (!std::filesystem::is_directory(dir)) {
        SKIP("no configured carvers at " << dir << "; run tools/fetch-vanilla");
    }
    std::map<std::string, CarverType> types;
    for (const auto& file : std::filesystem::directory_iterator(dir)) {
        const std::string name = "minecraft:" + file.path().stem().string();
        INFO(name);
        const ConfiguredCarver carver = ConfiguredCarver::fromJson(readJson(file.path()), name);
        types.emplace(name, carver.type);
    }
    CHECK(types == std::map<std::string, CarverType>{
                       {"minecraft:canyon", CarverType::Canyon},
                       {"minecraft:cave", CarverType::Cave},
                       {"minecraft:cave_extra_underground", CarverType::Cave},
                       {"minecraft:nether_cave", CarverType::NetherCave},
                   });
}

TEST_CASE("every vanilla biome's carvers field parses, in the shipped forms",
          "[conformance][carver]") {
    const std::filesystem::path dir = versionDir() / "worldgen" / "biome";
    if (!std::filesystem::is_directory(dir)) {
        SKIP("no biomes at " << dir << "; run tools/fetch-vanilla");
    }
    std::size_t biomes = 0;
    std::size_t bare = 0;
    std::size_t withCanyon = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir)) {
        const std::string name = "minecraft:" + file.path().stem().string();
        INFO(name);
        const nlohmann::json json = readJson(file.path());
        REQUIRE(json.contains("carvers"));
        const std::vector<CarverEntry> entries =
            stratum::carver::parseBiomeCarvers(json.at("carvers"), name);
        ++biomes;
        bare += json.at("carvers").is_string() ? 1U : 0U;
        // spec/carvers-b-spec.md Q1.1: in every shipped overworld list the
        // canyon is third, k = 2.
        for (std::size_t k = 0; k < entries.size(); ++k) {
            const auto* ref = std::get_if<ResourceLocation>(&entries.at(k));
            if (ref != nullptr && *ref == ResourceLocation::parse("minecraft:canyon")) {
                CHECK(k == 2U);
                ++withCanyon;
            }
        }
    }
    CHECK(biomes > 60U);
    // The shipped Nether biomes write a bare id (spec B-Q11.1).
    CHECK(bare == 5U);
    CHECK(withCanyon > 0U);
}

TEST_CASE("vanilla's carver-replaceable tags resolve to the spec's block lists",
          "[conformance][carver][tags]") {
    const std::filesystem::path tagDir = versionDir() / "tags" / "block";
    if (!std::filesystem::is_directory(tagDir)) {
        SKIP("no block tags at " << tagDir << "; run tools/fetch-vanilla");
    }
    const BlockTags tags = BlockTags::load(tagDir);
    const nlohmann::json constants =
        readJson(std::filesystem::path(STRATUM_SPEC_DIR) / "carvers-a-constants.json");
    const nlohmann::json& expected = constants.at("resolved_replaceables");
    std::size_t compared = 0;
    for (const auto& [tag, blocks] : expected.items()) {
        if (!tag.starts_with('#')) {
            continue; // the entry's `claim`, beside the tags
        }
        INFO(tag);
        std::set<ResourceLocation> want;
        for (const nlohmann::json& b : blocks) {
            want.insert(ResourceLocation::parse(b.get<std::string>()));
        }
        CHECK(tags.resolve(ResourceLocation::parse(tag.substr(1))) == want);
        ++compared;
    }
    CHECK(compared == 2U);
    CHECK(tags.resolve(ResourceLocation::parse("minecraft:overworld_carver_replaceables")).size() ==
          52U);
    CHECK(tags.resolve(ResourceLocation::parse("minecraft:nether_carver_replaceables")).size() ==
          25U);
}
