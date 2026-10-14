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

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/version.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
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
    TempTree() : root_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(root_);
        for (const char* registry : {"density_function", "noise", "noise_settings", "biome"}) {
            std::filesystem::create_directories(root_ / "worldgen" / registry);
        }
        std::filesystem::create_directories(root_ / "biome_parameters" / "minecraft");
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    void write(const char* registry, std::string_view name, std::string_view json) const {
        std::ofstream out(root_ / "worldgen" / registry / (std::string(name) + ".json"));
        out << json;
    }

    void writeBiomeParameters(std::string_view name, std::string_view json) const {
        std::ofstream out(root_ / "biome_parameters" / "minecraft" / (std::string(name) + ".json"));
        out << json;
    }

    [[nodiscard]] Pack pack() const { return Pack::open(root_ / "worldgen"); }

    [[nodiscard]] std::filesystem::path biomeParametersDir() const {
        return root_ / "biome_parameters";
    }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-freeze-test");
    }

    std::filesystem::path root_;
};

/// A pipeline with one of everything the format has to carry: a spline, a
/// noise reference, a noise written inline, a selector, a nested spline
/// point, and settings with both a block state that has properties and one
/// that does not.
[[nodiscard]] Pipeline buildPipeline(const TempTree& tree) {
    tree.write("noise", "test", R"({"firstOctave":-4,"amplitudes":[1.0,0.0,2.5]})");
    tree.write("density_function", "field",
               R"({"type":"minecraft:noise","noise":"test","xz_scale":0.25,"y_scale":0.125})");
    // The other spelling of a noise field. It belongs in the fixture rather
    // than in a test of its own: a blob that lost it would come back as a
    // node that samples nothing, and every round-trip check here should be
    // looking for that.
    tree.write("density_function", "inlined",
               R"({"type":"minecraft:shift_b",
                   "argument":{"firstOctave":-6,"amplitudes":[0.5,0.0,1.5]}})");
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

    // A noise nothing in the graph names, as surface rules' noises are: the
    // freeze has to carry it anyway.
    tree.write("noise", "surface_only", R"({"firstOctave":-2,"amplitudes":[1.0]})");

    // Temperatures chosen to have bit patterns a float32 round trip through
    // text would disturb.
    tree.write("biome", "plains", R"({"temperature":0.8})");
    tree.write("biome", "snowy_plains", R"({"temperature":-0.1})");
    // Both spellings of an axis, a nonzero offset, and a tie-sensitive order.
    tree.writeBiomeParameters("overworld", R"({"biomes":[
        {"biome":"minecraft:plains","parameters":{"temperature":[-0.15,0.2],"humidity":0.1,
         "continentalness":[-0.11,0.55],"erosion":[-1.0,1.0],"depth":0.0,"weirdness":[-1.0,1.0],
         "offset":0.0}},
        {"biome":"minecraft:snowy_plains","parameters":{"temperature":[-1.0,-0.45],
         "humidity":[-1.0,-0.35],"continentalness":[-0.11,0.3],"erosion":[0.45,0.55],
         "depth":1.0,"weirdness":[0.0,0.0],"offset":0.375}}]})");

    return stratum::freeze::resolve(tree.pack(), tree.biomeParametersDir());
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
        CHECK(a.inlineNoise == b.inlineNoise);
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
            REQUIRE(a.points[k].value.has_value() == b.points[k].value.has_value());
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

    REQUIRE(after.biomeParameters.size() == before.biomeParameters.size());
    for (const auto& [id, list] : before.biomeParameters) {
        REQUIRE(after.biomeParameters.contains(id));
        const auto& other = after.biomeParameters.at(id);
        CAPTURE(id.toString());
        REQUIRE(other.size() == list.size());
        for (std::size_t k = 0; k < list.size(); ++k) {
            const auto& a = list.entries()[k].parameters;
            const auto& b = other.entries()[k].parameters;
            CAPTURE(k);
            CHECK(other.entries()[k].biome == list.entries()[k].biome);
            for (const auto& [x, y] :
                 {std::pair{a.temperature, b.temperature}, std::pair{a.humidity, b.humidity},
                  std::pair{a.continentalness, b.continentalness}, std::pair{a.erosion, b.erosion},
                  std::pair{a.depth, b.depth}, std::pair{a.weirdness, b.weirdness}}) {
                CHECK(bits(x.min) == bits(y.min));
                CHECK(bits(x.max) == bits(y.max));
            }
            CHECK(bits(a.offset) == bits(b.offset));
        }
    }
    REQUIRE(after.biomeTemperatures.size() == before.biomeTemperatures.size());
    for (const auto& [id, temperature] : before.biomeTemperatures.entries()) {
        CAPTURE(id.toString());
        CHECK(std::bit_cast<std::uint32_t>(after.biomeTemperatures.at(id)) ==
              std::bit_cast<std::uint32_t>(temperature));
    }
}

} // namespace

TEST_CASE("a frozen pipeline comes back exactly as it went in", "[freeze]") {
    const TempTree tree;
    const Pipeline before = buildPipeline(tree);
    // What resolve() is for: everything generation reads, not just the graph.
    CHECK(before.noises.contains(ResourceLocation::parse("minecraft:surface_only")));
    CHECK(before.biomeParameters.size() == 1U);
    CHECK(before.biomeTemperatures.size() == 2U);
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

/// How many single-byte corruptions of the payload are refused with a
/// message mentioning @p what. A scan rather than a computed offset: to aim
/// at one field the test would have to know the payload's byte layout, and
/// that layout is the thing under test.
[[nodiscard]] std::size_t refusalsMentioning(const std::vector<std::byte>& good,
                                             std::size_t payload, std::string_view what) {
    std::size_t caught = 0;
    for (std::size_t at = payload; at < good.size(); ++at) {
        std::vector<std::byte> blob = good;
        blob[at] = std::byte{200};
        reseal(blob);
        try {
            static_cast<void>(stratum::freeze::read(blob));
        } catch (const FreezeError& error) {
            if (std::string(error.what()).find(what) != std::string::npos) {
                ++caught;
            }
        }
    }
    return caught;
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
        // A reader that cast the stored byte straight to NodeType would
        // accept every one of these and build a graph out of nonsense.
        CHECK(refusalsMentioning(good, payload, "does not know") > 0U);
    }

    SECTION("a stored biome parameter list that is not one") {
        // Counts, identifiers and ranges in the section format 3 added are
        // read with the same suspicion as everything before them.
        CHECK(refusalsMentioning(good, payload, "biome parameter list") > 0U);
    }

    SECTION("a stored noise outside the bounds a pack's is held to") {
        // A firstOctave the loader would refuse used to thaw unchecked, and
        // 2^31 - 1 overflowed the octave's name (SPEC §8). Byte 200 in a
        // first octave's low byte is 200 or more: past the bound.
        CHECK(refusalsMentioning(good, payload, "\"firstOctave\" is") > 0U);
    }

    SECTION("a stored geometry outside the bounds a pack's is held to") {
        CHECK(refusalsMentioning(good, payload, "stored noise settings") > 0U);
    }

    SECTION("a noise field tagged as neither spelling") {
        // The field is a union, so the tag has three legal values and 253
        // illegal ones. A reader that treated anything non-zero as "an
        // identifier follows" would read an inline noise's first octave as
        // the length of a string.
        CHECK(refusalsMentioning(good, payload, "not one of the two spellings") > 0U);
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

namespace {

/// The bytes the blob writes for a count or an index: four, little-endian.
[[nodiscard]] std::vector<std::byte> le32(std::uint32_t value) {
    std::vector<std::byte> bytes;
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
    return bytes;
}

/// The bytes the blob writes for a double: its bit pattern, little-endian.
[[nodiscard]] std::vector<std::byte> le64(double value) {
    const auto pattern = std::bit_cast<std::uint64_t>(value);
    std::vector<std::byte> bytes;
    for (std::uint64_t shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((pattern >> shift) & 0xFFU));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> joined(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> bytes;
    for (const std::vector<std::byte>& part : parts) {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    return bytes;
}

/// Every offset at which @p needle starts in @p blob.
[[nodiscard]] std::vector<std::size_t> offsetsOf(const std::vector<std::byte>& blob,
                                                 const std::vector<std::byte>& needle) {
    std::vector<std::size_t> found;
    for (std::size_t at = 0; at + needle.size() <= blob.size(); ++at) {
        if (std::equal(needle.begin(), needle.end(),
                       blob.begin() + static_cast<std::ptrdiff_t>(at))) {
            found.push_back(at);
        }
    }
    return found;
}

/// @p blob with @p needle — which must occur in it exactly once — replaced by
/// @p replacement, and the payload's length and hash rewritten to match: a
/// blob every outer check accepts, wrong only where the test means it to be.
[[nodiscard]] std::vector<std::byte> respliced(std::vector<std::byte> blob,
                                               const std::vector<std::byte>& needle,
                                               const std::vector<std::byte>& replacement) {
    const std::vector<std::size_t> found = offsetsOf(blob, needle);
    REQUIRE(found.size() == 1U);
    const auto at = static_cast<std::ptrdiff_t>(found.front());
    blob.erase(blob.begin() + at, blob.begin() + at + static_cast<std::ptrdiff_t>(needle.size()));
    blob.insert(blob.begin() + at, replacement.begin(), replacement.end());
    const std::size_t payload = payloadOffset();
    const std::uint64_t length = blob.size() - payload;
    for (std::size_t i = 0; i < 8U; ++i) {
        blob[payload - 8U + i] = static_cast<std::byte>((length >> (i * 8U)) & 0xFFU);
    }
    reseal(blob);
    return blob;
}

} // namespace

TEST_CASE("a sealed blob whose node is not its type's shape is refused by name", "[freeze][ub]") {
    // A blob's counts are whatever its bytes say, and the interpreter indexes
    // a node's arguments and parameters by position. Each blob below is
    // sealed — its length and hash agree with it — and differs from one this
    // build wrote only in one node's counts; read unchecked, each evaluated
    // past the end of a vector (SPEC §8). The values are chosen to have bit
    // patterns that occur once in the blob, so the edit can find its node
    // without the test knowing the payload's layout.
    constexpr double kConstant = 0.3141592653589793;
    constexpr double kFromY = -17.25;
    constexpr double kToY = 23.75;
    constexpr double kFromValue = 0.1234567890123;
    constexpr double kToValue = -0.9876543210987;
    constexpr double kMin = -0.5555555555555;
    constexpr double kMax = 0.4444444444444;
    const TempTree tree;
    tree.write("density_function", "shaped", R"({"type":"minecraft:add",
        "argument1":0.3141592653589793,
        "argument2":{"type":"minecraft:abs","argument":{"type":"minecraft:clamp",
            "input":{"type":"minecraft:y_clamped_gradient","from_y":-17.25,"to_y":23.75,
                     "from_value":0.1234567890123,"to_value":-0.9876543210987},
            "min":-0.5555555555555,"max":0.4444444444444}}})");
    const std::vector<std::byte> good = stratum::freeze::write(buildPipeline(tree));
    const Pipeline thawed = stratum::freeze::read(good);
    // Where the clamp landed, for the abs that names it.
    std::uint32_t clampIndex = 0;
    for (std::size_t i = 0; i < thawed.graph.nodeCount(); ++i) {
        if (thawed.graph.node(static_cast<stratum::density::NodeIndex>(i)).type ==
            stratum::density::NodeType::Clamp) {
            clampIndex = static_cast<std::uint32_t>(i);
        }
    }

    SECTION("a constant with no parameter") {
        const auto blob = respliced(good, joined({le32(1), le64(kConstant)}), le32(0));
        CHECK_THROWS_WITH(stratum::freeze::read(blob),
                          ContainsSubstring("malformed") &&
                              ContainsSubstring("'minecraft:constant' carries 0 numeric "
                                                "parameter(s) where its type reads 1"));
    }

    SECTION("a y_clamped_gradient with none of its four") {
        const auto blob = respliced(
            good, joined({le32(4), le64(kFromY), le64(kToY), le64(kFromValue), le64(kToValue)}),
            le32(0));
        CHECK_THROWS_WITH(stratum::freeze::read(blob),
                          ContainsSubstring("'minecraft:y_clamped_gradient' carries 0 numeric "
                                            "parameter(s) where its type reads 4"));
    }

    SECTION("a clamp with no input") {
        // The clamp's record: one argument, the gradient's index, then its two
        // parameters. The argument goes; the parameters stay.
        const std::vector<std::byte> parameters = joined({le32(2), le64(kMin), le64(kMax)});
        const std::vector<std::size_t> at = offsetsOf(good, parameters);
        REQUIRE(at.size() == 1U);
        REQUIRE(at.front() >= 8U);
        const std::vector<std::byte> arguments(
            good.begin() + static_cast<std::ptrdiff_t>(at.front() - 8U),
            good.begin() + static_cast<std::ptrdiff_t>(at.front()));
        REQUIRE(std::equal(arguments.begin(), arguments.begin() + 4, le32(1).begin()));
        const auto blob =
            respliced(good, joined({arguments, parameters}), joined({le32(0), parameters}));
        CHECK_THROWS_WITH(stratum::freeze::read(blob),
                          ContainsSubstring("'minecraft:clamp' carries 0 argument(s) where its "
                                            "type reads 1"));
    }

    SECTION("an abs with no argument") {
        // The abs's whole record: its type, one argument naming the clamp, no
        // parameters, no noise, no spline and an empty selector.
        const auto record = [](const std::vector<std::byte>& arguments) {
            const auto type = static_cast<std::uint8_t>(stratum::density::NodeType::Abs);
            return joined({{static_cast<std::byte>(type)},
                           arguments,
                           le32(0),
                           {std::byte{0}, std::byte{0}},
                           le32(0)});
        };
        const auto blob =
            respliced(good, record(joined({le32(1), le32(clampIndex)})), record(le32(0)));
        CHECK_THROWS_WITH(stratum::freeze::read(blob),
                          ContainsSubstring("'minecraft:abs' carries 0 argument(s) where its "
                                            "type reads 1"));
    }

    SECTION("the blob as written is read") {
        CHECK(thawed.graph.nodeCount() > 5U);
    }

    SECTION("a value refusal is told apart from a malformed blob") {
        // A reversed clamp is well formed, and a build from before SPEC §8's
        // refusals froze one at this engine version: the message says so and
        // what to do, which "malformed" did not (SPEC §6).
        const auto blob = respliced(good, joined({le32(2), le64(kMin), le64(kMax)}),
                                    joined({le32(2), le64(kMax), le64(kMin)}));
        CHECK_THROWS_WITH(stratum::freeze::read(blob),
                          ContainsSubstring("carries a value this build refuses") &&
                              ContainsSubstring("'minecraft:clamp' has min") &&
                              ContainsSubstring("generate this world with that build") &&
                              !ContainsSubstring("malformed"));
    }
}

namespace {

/// `abs` wrapped @p levels times around @p innermost, as JSON text.
[[nodiscard]] std::string nestedAbs(std::size_t levels, std::string_view innermost) {
    std::string text;
    for (std::size_t i = 0; i < levels; ++i) {
        text += R"({"type":"minecraft:abs","argument":)";
    }
    text += innermost;
    text.append(levels, '}');
    return text;
}

/// The bytes the blob writes for a string: its length, then its bytes.
[[nodiscard]] std::vector<std::byte> stored(std::string_view text) {
    std::vector<std::byte> bytes = le32(static_cast<std::uint32_t>(text.size()));
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

/// The whole record the blob writes for an `abs` over @p argument: its type,
/// one argument, no parameters, no noise, no spline, an empty selector.
[[nodiscard]] std::vector<std::byte> absRecord(std::uint32_t argument) {
    const auto type = static_cast<std::uint8_t>(stratum::density::NodeType::Abs);
    return joined({{static_cast<std::byte>(type)},
                   le32(1),
                   le32(argument),
                   le32(0),
                   {std::byte{0}, std::byte{0}},
                   le32(0)});
}

} // namespace

TEST_CASE("a sealed blob deeper than kMaxDepth, or with a cycle, is refused by name",
          "[freeze][ub]") {
    // A resealed blob's 20 000-node chain into a router entry ran the
    // interpreter's recursion out (SPEC §8). Past density::kMaxDepth a graph
    // is refused as a value this build refuses — a build from before the
    // bound may have frozen one (§6) — and evaluated at it; a cycle, which no
    // build writes, is malformed. Each blob is sealed and differs from one
    // this build wrote in one index, so its length is unchanged.
    constexpr double kDeep = 0.2718281828459045;
    constexpr double kFits = 0.5772156649015329;
    constexpr double kPast = 0.6180339887498949;
    const TempTree tree;
    // Each inside data::kMaxJsonDepth, as a pack's file has to be.
    tree.write("density_function", "deep_a", nestedAbs(128, "0.2718281828459045"));
    tree.write("density_function", "deep_b", nestedAbs(127, "0.5772156649015329"));
    tree.write("density_function", "deep_c", nestedAbs(128, "0.6180339887498949"));
    const std::vector<std::byte> good = stratum::freeze::write(buildPipeline(tree));
    const Pipeline thawed = stratum::freeze::read(good);
    const auto indexOfConstant = [&thawed](double value) {
        std::optional<std::uint32_t> found;
        for (std::size_t i = 0; i < thawed.graph.nodeCount(); ++i) {
            const auto& node = thawed.graph.node(static_cast<stratum::density::NodeIndex>(i));
            if (node.type == stratum::density::NodeType::Constant &&
                bits(node.parameters.front()) == bits(value)) {
                found = static_cast<std::uint32_t>(i);
            }
        }
        REQUIRE(found.has_value());
        return found.value_or(0U);
    };
    const std::uint32_t deepTop = thawed.graph.rootOf(ResourceLocation::parse("deep_a"));
    REQUIRE(deepTop == indexOfConstant(kDeep) + 128U);
    // The `abs` around a function's constant made to name deep_a's top, 129
    // deep, instead: deep_b comes to 256, deep_c to 257.
    const auto relinked = [&](double constant) {
        return respliced(good, absRecord(indexOfConstant(constant)), absRecord(deepTop));
    };

    SECTION("at the bound, read and evaluated") {
        const Pipeline fits = stratum::freeze::read(relinked(kFits));
        const auto noises = stratum::density::NoiseRegistry::create(
            std::map<ResourceLocation, stratum::density::NoiseParameters>{},
            std::vector<ResourceLocation>{}, 0, stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(fits.graph, noises);
        const stratum::density::NodeIndex root =
            fits.graph.rootOf(ResourceLocation::parse("deep_b"));
        CHECK_NOTHROW(interpreter.requireEvaluable(root));
        CHECK(bits(interpreter.evaluate(root, stratum::density::Point{.x = 0, .y = 0, .z = 0})) ==
              bits(kDeep));
    }

    SECTION("one past it, refused as a value this build refuses") {
        CHECK_THROWS_WITH(stratum::freeze::read(relinked(kPast)),
                          ContainsSubstring("carries a value this build refuses") &&
                              ContainsSubstring("('minecraft:abs') sits 257 nodes and splines "
                                                "deep, past the 256") &&
                              ContainsSubstring("generate this world with that build") &&
                              !ContainsSubstring("malformed"));
    }

    SECTION("a spline whose coordinate leads back to the node holding it, malformed") {
        // `curved`'s outer spline is sampled along `field`; made to sample
        // along `curved` itself, evaluating it would recurse without end.
        const std::uint32_t field = thawed.graph.rootOf(ResourceLocation::parse("field"));
        const std::uint32_t curved = thawed.graph.rootOf(ResourceLocation::parse("curved"));
        const auto outer = [](std::uint32_t coordinate) {
            return joined({le32(coordinate), le32(2), le64(-1.0), le64(0.25)});
        };
        CHECK_THROWS_WITH(
            stratum::freeze::read(respliced(good, outer(field), outer(curved))),
            ContainsSubstring("malformed") &&
                ContainsSubstring("leads back to frozen node " + std::to_string(curved)) &&
                ContainsSubstring("cycle"));
    }

    SECTION("stored surface rules or spawn targets nested past data::kMaxJsonDepth") {
        // A pack's files are held to it as they are read; a blob stores these
        // two as text, and copying what that text parses to recurses once per
        // level, as the settings are copied into a dimension.
        const std::string deepArrays = std::string(600, '[') + "1" + std::string(600, ']');
        CHECK_THROWS_WITH(
            stratum::freeze::read(
                respliced(good, stored(R"({"type":"minecraft:bandlands"})"),
                          stored(R"({"type":"minecraft:bandlands","deep":)" + deepArrays + "}"))),
            ContainsSubstring("carries a value this build refuses") &&
                ContainsSubstring("\"surface_rule\" that nests arrays and objects 601 deep") &&
                !ContainsSubstring("malformed"));
        CHECK_THROWS_WITH(
            stratum::freeze::read(respliced(good, stored("[1,2,3]"), stored(deepArrays))),
            ContainsSubstring("\"spawn_target\" that nests arrays and objects 600 deep"));
    }
}

TEST_CASE("a thawed pipeline builds the same noises its pack would", "[freeze]") {
    // SPEC §6 is only honoured if generation can run from the blob alone, and
    // that starts with the noises: built from stored parameters they have to
    // be the very same noises the pack builds, seeded the same way.
    const TempTree tree;
    const Pipeline thawed = stratum::freeze::read(stratum::freeze::write(buildPipeline(tree)));
    const Pack pack = tree.pack(); // after buildPipeline has written the tree

    const std::vector<ResourceLocation> wanted = {
        ResourceLocation::parse("minecraft:test"),
        ResourceLocation::parse("minecraft:surface_only")};
    const auto fromPack = stratum::density::NoiseRegistry::create(
        pack, wanted, 1234567, stratum::density::RandomSource::Xoroshiro);
    const auto fromBlob = stratum::density::NoiseRegistry::create(
        thawed.noises, wanted, 1234567, stratum::density::RandomSource::Xoroshiro);
    for (const ResourceLocation& id : wanted) {
        CAPTURE(id.toString());
        for (const double at : {-1000.25, 0.0, 17.5, 30000.125}) {
            CHECK(bits(fromPack.get(id).sample(at, at * 0.5, -at)) ==
                  bits(fromBlob.get(id).sample(at, at * 0.5, -at)));
        }
    }

    CHECK_THROWS_WITH(stratum::density::NoiseRegistry::create(
                          thawed.noises, std::vector{ResourceLocation::parse("minecraft:absent")},
                          1, stratum::density::RandomSource::Xoroshiro),
                      ContainsSubstring("minecraft:absent"));
    CHECK_THROWS_WITH(stratum::density::NoiseRegistry::create(
                          thawed.noises, wanted, 1, stratum::density::RandomSource::Legacy),
                      ContainsSubstring("legacy_random_source"));
}
