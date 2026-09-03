// Stratum — freezing a pipeline into a world, and refusing to thaw a wrong one.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §6 asks for three things and this file checks all three: that a
// frozen pipeline comes back exactly as it went in, that freezing the same
// pipeline twice gives the same bytes, and that a blob this build cannot
// honour is refused loudly rather than generated from.
//
// The refusals are most of the file, and deliberately so. A round trip that
// works is pleasant; a world that opens when it should not have is a seam in
// somebody's terrain that no later fix can remove.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/version.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::freeze::FreezeError;
using stratum::freeze::Pipeline;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void write(const char* registry, std::string_view name, std::string_view json) const {
        std::ofstream out(path_ / registry / (std::string(name) + ".json"));
        out << json;
    }

    [[nodiscard]] Pack pack() const { return Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-freeze-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

/// A pipeline with one of everything the format has to carry: a spline, a
/// noise reference, a selector, a nested spline point, and settings with
/// both a block state that has properties and one that does not.
[[nodiscard]] Pipeline buildPipeline(const TempTree& tree) {
    tree.write("noise", "test", R"({"firstOctave":-4,"amplitudes":[1.0,0.0,2.5]})");
    tree.write("density_function", "field",
               R"({"type":"minecraft:noise","noise":"test","xz_scale":0.25,"y_scale":0.125})");
    tree.write("density_function", "curved", R"({"type":"minecraft:spline","spline":{
        "coordinate":"field",
        "points":[{"location":-1.0,"value":0.5,"derivative":0.25},
                  {"location":1.0,"derivative":-0.5,
                   "value":{"coordinate":"field",
                            "points":[{"location":0.0,"value":-2.0,"derivative":0.0}]}}]}})");

    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(
            static_cast<stratum::settings::RouterEntry>(i)))] = "curved";
    }
    const nlohmann::json settingsJson{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}, {"Properties", {{"level", "0"}}}}},
        {"sea_level", 63},
        {"disable_mob_generation", false},
        {"aquifers_enabled", true},
        {"ore_veins_enabled", false},
        {"legacy_random_source", true},
        {"noise", {{"min_y", -64}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array({1, 2, 3})},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
    tree.write("noise_settings", "overworld", settingsJson.dump());

    const Pack pack = tree.pack();
    stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);

    Pipeline pipeline;
    pipeline.settings = std::move(loaded.settings);
    pipeline.graph = std::move(loaded.graph);
    for (const ResourceLocation& id : pipeline.graph.referencedNoises()) {
        pipeline.noises.emplace(id, stratum::density::NoiseParameters::fromJson(
                                        pack.find(stratum::data::Registry::Noise, id)->json, id));
    }
    return pipeline;
}

void expectSame(const Pipeline& before, const Pipeline& after) {
    REQUIRE(after.graph.nodeCount() == before.graph.nodeCount());
    REQUIRE(after.graph.splineCount() == before.graph.splineCount());
    for (std::size_t i = 0; i < before.graph.nodeCount(); ++i) {
        const auto index = static_cast<stratum::density::NodeIndex>(i);
        const auto& a = before.graph.node(index);
        const auto& b = after.graph.node(index);
        CAPTURE(i);
        CHECK(a.type == b.type);
        CHECK(a.arguments == b.arguments);
        REQUIRE(a.parameters.size() == b.parameters.size());
        for (std::size_t k = 0; k < a.parameters.size(); ++k) {
            // Bit patterns, not values: a freeze that lost the last bit of a
            // spline knot would still round-trip to something that looks
            // right and generates a different world.
            CHECK(bits(a.parameters[k]) == bits(b.parameters[k]));
        }
        CHECK(a.noise == b.noise);
        CHECK(a.spline == b.spline);
        CHECK(a.selector == b.selector);
    }
    for (std::size_t i = 0; i < before.graph.splineCount(); ++i) {
        const auto index = static_cast<stratum::density::SplineIndex>(i);
        const auto& a = before.graph.spline(index);
        const auto& b = after.graph.spline(index);
        CAPTURE(i);
        CHECK(a.coordinate == b.coordinate);
        REQUIRE(a.points.size() == b.points.size());
        for (std::size_t k = 0; k < a.points.size(); ++k) {
            CHECK(bits(a.points[k].location) == bits(b.points[k].location));
            CHECK(bits(a.points[k].derivative) == bits(b.points[k].derivative));
            CHECK(a.points[k].value.has_value() == b.points[k].value.has_value());
            if (a.points[k].value.has_value()) {
                CHECK(bits(*a.points[k].value) == bits(*b.points[k].value));
            } else {
                CHECK(a.points[k].nested == b.points[k].nested);
            }
        }
    }
    CHECK(after.graph.roots() == before.graph.roots());

    REQUIRE(after.noises.size() == before.noises.size());
    for (const auto& [id, parameters] : before.noises) {
        REQUIRE(after.noises.contains(id));
        const auto& other = after.noises.at(id);
        CAPTURE(id.toString());
        CHECK(other.firstOctave == parameters.firstOctave);
        REQUIRE(other.amplitudes.size() == parameters.amplitudes.size());
        for (std::size_t k = 0; k < parameters.amplitudes.size(); ++k) {
            CHECK(bits(other.amplitudes[k]) == bits(parameters.amplitudes[k]));
        }
    }

    REQUIRE(after.settings.size() == before.settings.size());
    for (const auto& [id, one] : before.settings) {
        REQUIRE(after.settings.contains(id));
        const auto& other = after.settings.at(id);
        CAPTURE(id.toString());
        CHECK(other.id == one.id);
        CHECK(other.defaultBlock == one.defaultBlock);
        CHECK(other.defaultFluid == one.defaultFluid);
        CHECK(other.seaLevel == one.seaLevel);
        CHECK(other.disableMobGeneration == one.disableMobGeneration);
        CHECK(other.aquifersEnabled == one.aquifersEnabled);
        CHECK(other.oreVeinsEnabled == one.oreVeinsEnabled);
        CHECK(other.legacyRandomSource == one.legacyRandomSource);
        CHECK(other.geometry == one.geometry);
        CHECK(other.router.entries == one.router.entries);
        // Kept as text and not interpreted, so what matters is that it comes
        // back meaning the same thing.
        CHECK(other.surfaceRule == one.surfaceRule);
        CHECK(other.spawnTarget == one.spawnTarget);
    }
}

} // namespace

TEST_CASE("a frozen pipeline comes back exactly as it went in", "[freeze]") {
    const TempTree tree;
    const Pipeline before = buildPipeline(tree);
    const std::vector<std::byte> blob = stratum::freeze::write(before);
    CHECK(blob.size() > 100U);

    const Pipeline after = stratum::freeze::read(blob);
    expectSame(before, after);
}

TEST_CASE("freezing the same pipeline twice gives the same bytes", "[freeze]") {
    const TempTree tree;
    const Pipeline pipeline = buildPipeline(tree);

    // SPEC §5.6: engine updates must reproduce stored pipelines
    // byte-identically. That starts with one build reproducing its own.
    CHECK(stratum::freeze::write(pipeline) == stratum::freeze::write(pipeline));

    // And a round trip is a fixed point: writing what was read gives the
    // same bytes again, which is what makes a stored blob comparable across
    // versions at all.
    const std::vector<std::byte> once = stratum::freeze::write(pipeline);
    CHECK(stratum::freeze::write(stratum::freeze::read(once)) == once);
}

TEST_CASE("a blob says what made it", "[freeze]") {
    const TempTree tree;
    const std::vector<std::byte> blob = stratum::freeze::write(buildPipeline(tree));
    const stratum::freeze::Provenance provenance = stratum::freeze::inspect(blob);

    CHECK(provenance.blobFormat == stratum::freeze::kBlobFormat);
    CHECK(provenance.pipelineEngineVersion == stratum::kPipelineEngineVersion);
    CHECK(provenance.minecraftVersion == stratum::kMinecraftVersion);
    CHECK(provenance.packFormatMajor == stratum::kPackFormatMajor);
    CHECK(provenance.packFormatMinor == stratum::kPackFormatMinor);
}

TEST_CASE("a pipeline this build cannot honour is refused, not generated from", "[freeze]") {
    const TempTree tree;
    const std::vector<std::byte> good = stratum::freeze::write(buildPipeline(tree));

    SECTION("something that is not a blob at all") {
        const std::vector<std::byte> junk(64, std::byte{0x41});
        CHECK_THROWS_WITH(stratum::freeze::read(junk), ContainsSubstring("STRTMPIP"));
        CHECK_THROWS_WITH(stratum::freeze::read(std::vector<std::byte>{}),
                          ContainsSubstring("too few"));
    }

    SECTION("a container format this build does not write") {
        std::vector<std::byte> blob = good;
        blob[8] = std::byte{99}; // the format word, straight after the magic
        CHECK_THROWS_WITH(stratum::freeze::read(blob), ContainsSubstring("container format"));
    }

    SECTION("an engine version that may generate differently") {
        std::vector<std::byte> blob = good;
        blob[12] = std::byte{static_cast<std::uint8_t>(stratum::kPipelineEngineVersion + 1)};
        // The message has to name both versions and say what to do, because
        // "cannot load world" leaves someone with a world and no next step.
        CHECK_THROWS_WITH(stratum::freeze::read(blob),
                          ContainsSubstring("pipeline engine version") &&
                              ContainsSubstring("seam") && ContainsSubstring("SPEC §6"));
    }

    SECTION("a payload that has been edited") {
        std::vector<std::byte> blob = good;
        // Flip a bit deep in the payload, well past the header.
        blob[blob.size() - 8] ^= std::byte{0x01};
        CHECK_THROWS_WITH(stratum::freeze::read(blob), ContainsSubstring("content hash"));
    }

    SECTION("a truncated file") {
        std::vector<std::byte> blob = good;
        blob.resize(blob.size() - 4);
        CHECK_THROWS_WITH(stratum::freeze::read(blob), ContainsSubstring("truncated"));
    }

    SECTION("a file with something appended") {
        std::vector<std::byte> blob = good;
        blob.push_back(std::byte{0});
        CHECK_THROWS_WITH(stratum::freeze::read(blob), ContainsSubstring("appended"));
    }
}

/// Where the payload starts, computed the way the header is laid out. A
/// test that edits a payload has to re-seal the hash, or the outer guard
/// catches it first and the inner ones are never reached.
[[nodiscard]] std::size_t payloadOffset() {
    return 8U                                       // magic
           + 4U + 4U                                // container format, engine version
           + 4U + stratum::kMinecraftVersion.size() // the pinned version, length-prefixed
           + 4U + 4U                                // pack format major, minor
           + 16U                                    // content hash
           + 8U;                                    // payload length
}

void reseal(std::vector<std::byte>& blob) {
    const std::size_t at = payloadOffset();
    const stratum::hash::Md5Digest digest =
        stratum::hash::md5(std::span<const std::byte>(blob).subspan(at));
    for (std::size_t i = 0; i < digest.size(); ++i) {
        blob[at - 8U - 16U + i] = static_cast<std::byte>(digest[i]);
    }
}

TEST_CASE("the checks behind the hash catch what the hash would have caught", "[freeze]") {
    const TempTree tree;
    const std::vector<std::byte> good = stratum::freeze::write(buildPipeline(tree));

    // The hash is the outer guard and would stop every edit below on its own.
    // These re-seal it deliberately, so that the inner checks are exercised
    // rather than merely present: defence in depth is only depth if the
    // inner layer works.
    const std::size_t payload = payloadOffset();
    REQUIRE(payload < good.size());
    { // Editing without resealing is caught by the hash, as it should be.
        std::vector<std::byte> blob = good;
        blob[payload] ^= std::byte{0xFF};
        CHECK_THROWS_WITH(stratum::freeze::read(blob), ContainsSubstring("content hash"));
    }

    SECTION("a node type this build does not know") {
        // Find the node table by walking: it follows the noise table, whose
        // shape the test knows because it built the pipeline. Simpler to
        // scan for the first byte whose corruption trips the type check.
        std::size_t caught = 0;
        for (std::size_t at = payload; at < good.size(); ++at) {
            std::vector<std::byte> blob = good;
            blob[at] = std::byte{200};
            reseal(blob);
            try {
                static_cast<void>(stratum::freeze::read(blob));
            } catch (const FreezeError& error) {
                if (std::string(error.what()).find("does not know") != std::string::npos) {
                    ++caught;
                }
            }
        }
        // A reader that cast the stored byte straight to NodeType would
        // accept every one of these and build a graph out of nonsense.
        CHECK(caught > 0U);
    }

    SECTION("a payload longer than its own contents") {
        // The declared length and the hash both agree with the file; only
        // the reader's own bookkeeping notices that the pipeline finished
        // before the payload did. Without this the exhausted() check would
        // be unreachable behind the length check and untested.
        std::vector<std::byte> blob = good;
        blob.push_back(std::byte{0});
        const std::uint64_t length = blob.size() - payload;
        for (std::size_t i = 0; i < 8U; ++i) {
            blob[payload - 8U + i] = static_cast<std::byte>((length >> (i * 8U)) & 0xFFU);
        }
        reseal(blob);
        CHECK_THROWS_WITH(stratum::freeze::read(blob), ContainsSubstring("trailing bytes"));
    }

    SECTION("an index that points past the end") {
        std::size_t caught = 0;
        for (std::size_t at = payload; at + 4U < good.size(); ++at) {
            std::vector<std::byte> blob = good;
            // A plausible-looking but far too large index.
            blob[at] = std::byte{0xFF};
            blob[at + 1U] = std::byte{0xFF};
            reseal(blob);
            try {
                static_cast<void>(stratum::freeze::read(blob));
            } catch (const FreezeError& error) {
                const std::string message = error.what();
                if (message.find("does not exist") != std::string::npos ||
                    message.find("not before it") != std::string::npos ||
                    message.find("is not a node") != std::string::npos ||
                    message.find("not a pipeline") != std::string::npos) {
                    ++caught;
                }
            }
        }
        CHECK(caught > 0U);
    }
}
