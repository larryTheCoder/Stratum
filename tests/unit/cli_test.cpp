// Stratum — CLI end-to-end tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The commands are thin, but their contract — which exit code means what — is
// what scripts and CI depend on, so it is tested against the real binary
// rather than the functions behind it.

#include "support/cli_process.hpp"
#include "support/region_builder.hpp"
#include "support/temp_path.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using stratum::test::CliResult;
using stratum::test::RegionBuilder;
using stratum::test::SectionSpec;

[[nodiscard]] CliResult runCli(const std::string& arguments) {
    return stratum::test::runCli(STRATUM_CLI_PATH, arguments);
}

[[nodiscard]] SectionSpec stoneSection(bool withDirt) {
    SectionSpec section;
    section.y = 0;
    section.palette = {"minecraft:stone", "minecraft:dirt"};
    section.blocks.assign(stratum::chunk::kBlocksPerSection, 0);
    if (withDirt) {
        section.blocks[(2U * 256U) + (3U * 16U) + 4U] = 1;
    }
    return section;
}

[[nodiscard]] std::filesystem::path writeRegion(const std::string& stem, bool withDirt) {
    RegionBuilder builder;
    builder.addChunk(1, 1, {stoneSection(withDirt)});
    const std::filesystem::path path = stratum::test::tempPath(stem, ".mca");
    builder.writeTo(path);
    return path;
}

/// The same stone chunk, every one of its biome cells @p biome.
[[nodiscard]] std::filesystem::path writeBiomeRegion(const std::string& stem,
                                                     const std::string& biome) {
    SectionSpec section = stoneSection(false);
    section.biomePalette = {biome};
    RegionBuilder builder;
    builder.addChunk(1, 1, {section});
    const std::filesystem::path path = stratum::test::tempPath(stem, ".mca");
    builder.writeTo(path);
    return path;
}

/// A minimal worldgen tree on disk, so the density render path can be driven
/// through the real binary without the vanilla fixtures.
class TempPack {
public:
    explicit TempPack(bool withUnevaluable = true)
        : path_(stratum::test::tempPath(withUnevaluable ? "stratum-cli-pack"
                                                        : "stratum-cli-pack-clean")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
        write("noise", "test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
        write("density_function", "field",
              R"({"type":"minecraft:noise","noise":"test","xz_scale":1.0,"y_scale":0.0})");
        if (withUnevaluable) {
            // slide, because it is still refused. This used to be
            // end_islands, which is now settled and evaluates; before that it
            // was
            // old_blended_noise, which now evaluates.
            write("density_function", "blended", R"({"type":"minecraft:slide","argument":0.0})");
        }
    }

    TempPack(const TempPack&) = delete;
    TempPack& operator=(const TempPack&) = delete;

    ~TempPack() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void write(const char* registry, const char* name, const char* json) const {
        std::ofstream out(path_ / registry / (std::string(name) + ".json"));
        out << json;
    }

    std::filesystem::path path_;
};

/// A version root as tools/fetch-vanilla lays one out — `worldgen/` and
/// `biome_parameters/` — small enough to generate from in a Debug build, so
/// `stratum generate` runs end to end without the vanilla fixtures. Every
/// dimension in it is shaped so that its output can be predicted block for
/// block:
///
///   overworld  stone below y 0, where its density falls to zero; no
///              aquifers, so water up to sea level 8 and air above; one
///              surface rule turning stone at y >= -2 into grass_block (a
///              state with a property); one biome, plains, everywhere.
///   end        legacy random source, end_stone below y 32 over a noise
///              height of 0..127 in a column of 0..255; the End's own biome
///              source, with erosion a constant 0.5 (end_highlands, off the
///              central island).
///   nether     a router whose final_density is `minecraft:slide`, which the
///              library cannot evaluate and refuses by name. (Not the
///              vanilla Nether's reason, its legacy named noises: that one is
///              meant to go away.)
///   amplified  the overworld's settings one section lower, y -80..303: out
///              of the column its dimension type stores.
class GeneratePack {
public:
    GeneratePack() : root_(stratum::test::tempPath("stratum-cli-generate-pack")) {
        std::filesystem::remove_all(root_);
        for (const char* registry : {"density_function", "noise", "noise_settings", "biome"}) {
            std::filesystem::create_directories(root_ / "worldgen" / registry);
        }
        std::filesystem::create_directories(root_ / "biome_parameters" / "minecraft");

        // The surface system's own noises, in case the tree is taken to read
        // a depth.
        write("noise", "surface", R"({"firstOctave":-6,"amplitudes":[1.0,1.0]})");
        write("noise", "surface_secondary", R"({"firstOctave":-6,"amplitudes":[1.0]})");
        write("biome", "plains", R"({"temperature":0.8})");

        const nlohmann::json grassOnTop = {
            {"type", "minecraft:condition"},
            {"if_true",
             {{"type", "minecraft:y_above"},
              {"anchor", {{"absolute", -2}}},
              {"surface_depth_multiplier", 0},
              {"add_stone_depth", false}}},
            {"then_run",
             {{"type", "minecraft:block"},
              {"result_state",
               {{"Name", "minecraft:grass_block"}, {"Properties", {{"snowy", "false"}}}}}}}};
        const nlohmann::json stoneBelowZero = gradient(-64, 64);
        write("noise_settings", "overworld",
              settings({.minY = -64, .height = 384, .seaLevel = 8, .legacy = false}, stoneBelowZero,
                       nlohmann::json(0.0), grassOnTop, "minecraft:stone", "minecraft:water")
                  .dump());
        write("noise_settings", "amplified",
              settings({.minY = -80, .height = 384, .seaLevel = 8, .legacy = false}, stoneBelowZero,
                       nlohmann::json(0.0), grassOnTop, "minecraft:stone", "minecraft:water")
                  .dump());
        write("noise_settings", "end",
              settings({.minY = 0, .height = 128, .seaLevel = 0, .legacy = true}, gradient(0, 64),
                       nlohmann::json(0.5),
                       {{"type", "minecraft:block"},
                        {"result_state", {{"Name", "minecraft:end_stone"}}}},
                       "minecraft:end_stone", "minecraft:air")
                  .dump());
        write("noise_settings", "nether",
              settings({.minY = 0, .height = 128, .seaLevel = 32, .legacy = true},
                       {{"type", "minecraft:slide"}, {"argument", 0.0}}, nlohmann::json(0.0),
                       {{"type", "minecraft:block"},
                        {"result_state", {{"Name", "minecraft:netherrack"}}}},
                       "minecraft:netherrack", "minecraft:lava")
                  .dump());

        const std::string everywhere = R"("temperature":[-1.0,1.0],"humidity":[-1.0,1.0],
            "continentalness":[-1.0,1.0],"erosion":[-1.0,1.0],"depth":[-1.0,1.0],
            "weirdness":[-1.0,1.0],"offset":0.0)";
        writeParameters("overworld", R"({"biomes":[{"biome":"minecraft:plains","parameters":{)" +
                                         everywhere + "}}]}");
        writeParameters("nether",
                        R"({"biomes":[{"biome":"minecraft:nether_wastes","parameters":{)" +
                            everywhere + "}}]}");
    }

    GeneratePack(const GeneratePack&) = delete;
    GeneratePack& operator=(const GeneratePack&) = delete;

    ~GeneratePack() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    /// The same pipeline frozen, as a world stores it (SPEC §6).
    [[nodiscard]] std::filesystem::path freeze() const {
        const std::vector<std::byte> blob = stratum::freeze::write(stratum::freeze::resolve(
            stratum::data::Pack::open(root_ / "worldgen"), root_ / "biome_parameters"));
        const std::filesystem::path path = root_ / "pipeline.blob";
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        return path;
    }

private:
    struct Shape {
        int minY = 0;
        int height = 0;
        int seaLevel = 0;
        bool legacy = false;
    };

    /// 1.0 at @p fromY falling to -1.0 at @p toY: solid below the midpoint.
    [[nodiscard]] static nlohmann::json gradient(int fromY, int toY) {
        return {{"type", "minecraft:y_clamped_gradient"},
                {"from_y", fromY},
                {"to_y", toY},
                {"from_value", 1.0},
                {"to_value", -1.0}};
    }

    [[nodiscard]] static nlohmann::json
    settings(const Shape& shape, const nlohmann::json& finalDensity, const nlohmann::json& erosion,
             const nlohmann::json& surfaceRule, const char* defaultBlock,
             const char* defaultFluid) {
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        router["final_density"] = finalDensity;
        router["erosion"] = erosion;
        return {{"default_block", {{"Name", defaultBlock}}},
                {"default_fluid", {{"Name", defaultFluid}}},
                {"sea_level", shape.seaLevel},
                {"disable_mob_generation", false},
                {"aquifers_enabled", false},
                {"ore_veins_enabled", false},
                {"legacy_random_source", shape.legacy},
                {"noise",
                 {{"min_y", shape.minY},
                  {"height", shape.height},
                  {"size_horizontal", 1},
                  {"size_vertical", 2}}},
                {"noise_router", router},
                {"spawn_target", nlohmann::json::array()},
                {"surface_rule", surfaceRule}};
    }

    void write(const char* registry, std::string_view name, std::string_view json) const {
        std::ofstream out(root_ / "worldgen" / registry / (std::string(name) + ".json"));
        out << json;
    }

    void writeParameters(std::string_view name, const std::string& json) const {
        std::ofstream out(root_ / "biome_parameters" / "minecraft" / (std::string(name) + ".json"));
        out << json;
    }

    std::filesystem::path root_;
};

/// A scratch output directory, removed with everything in it.
class TempDir {
public:
    explicit TempDir(std::string_view stem) : path_(stratum::test::tempPath(stem)) {
        std::filesystem::remove_all(path_);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

[[nodiscard]] std::filesystem::path regionPath(const std::filesystem::path& dir,
                                               std::int32_t regionX, std::int32_t regionZ) {
    return dir / ("r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca");
}

/// Chunk (@p chunkX, @p chunkZ) as `stratum generate` wrote it into @p dir.
[[nodiscard]] stratum::chunk::Chunk readGenerated(const std::filesystem::path& dir,
                                                  std::int32_t chunkX, std::int32_t chunkZ) {
    const auto region = stratum::region::RegionFile::open(
        regionPath(dir, stratum::javamath::floorDiv(chunkX, stratum::region::kChunksPerAxis),
                   stratum::javamath::floorDiv(chunkZ, stratum::region::kChunksPerAxis)));
    REQUIRE(region.hasChunk(chunkX, chunkZ));
    return stratum::chunk::Chunk::decode(stratum::nbt::read(region.readChunk(chunkX, chunkZ)).root);
}

[[nodiscard]] std::string blockAt(const stratum::chunk::Chunk& chunk, int x, int y, int z) {
    const stratum::chunk::BlockState* state = chunk.blockAt(x, y, z);
    return state == nullptr ? std::string("<no section>") : state->toString();
}

[[nodiscard]] std::string biomeAt(const stratum::chunk::Chunk& chunk, int x, int y, int z) {
    const std::string* biome = chunk.biomeAt(x, y, z);
    return biome == nullptr ? std::string("<no biomes>") : *biome;
}

[[nodiscard]] std::vector<char> bytesOf(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("diff exits 0 and says so when regions are identical", "[cli]") {
    const std::filesystem::path left = writeRegion("stratum-cli-a", false);
    const std::filesystem::path right = writeRegion("stratum-cli-b", false);

    const CliResult result = runCli("diff \"" + left.string() + "\" \"" + right.string() + "\"");
    CHECK(result.exitCode == 0);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("identical"));

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("diff exits 1 and names the block when regions differ", "[cli]") {
    const std::filesystem::path left = writeRegion("stratum-cli-c", false);
    const std::filesystem::path right = writeRegion("stratum-cli-d", true);

    const CliResult result = runCli("diff \"" + left.string() + "\" \"" + right.string() + "\"");
    // Exit 1 for "there are differences", as diff(1) does — a script can tell
    // disagreement apart from failure.
    CHECK(result.exitCode == 1);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("block (20, 2, 19)"));
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:stone"));
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:dirt"));

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("diff exits 1 and names the biome cell when only the biomes differ", "[cli]") {
    // Blocks identical, biomes not: before diff compared biomes this pair
    // read "regions are identical" and exited 0.
    const std::filesystem::path left = writeBiomeRegion("stratum-cli-h", "minecraft:the_end");
    const std::filesystem::path right = writeBiomeRegion("stratum-cli-i", "minecraft:plains");

    const CliResult result = runCli("diff \"" + left.string() + "\" \"" + right.string() + "\"");
    CHECK(result.exitCode == 1);
    CHECK_THAT(result.output,
               Catch::Matchers::ContainsSubstring("biome (16, 0, 16): minecraft:the_end != "
                                                  "minecraft:plains"));
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("0 differing block(s)"));
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("64 differing biome cell(s)"));
    CHECK_THAT(result.output, !Catch::Matchers::ContainsSubstring("identical"));

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("render writes a PNG", "[cli]") {
    const std::filesystem::path region = writeRegion("stratum-cli-e", false);
    const std::filesystem::path image = stratum::test::tempPath("stratum-cli-render", ".png");

    const CliResult result =
        runCli("render \"" + region.string() + "\" --out \"" + image.string() + "\"");
    CHECK(result.exitCode == 0);
    REQUIRE(std::filesystem::exists(image));

    std::ifstream stream(image, std::ios::binary);
    std::vector<char> header(8);
    stream.read(header.data(), 8);
    CHECK(static_cast<unsigned char>(header[0]) == 0x89U);
    CHECK(header[1] == 'P');
    CHECK(header[2] == 'N');
    CHECK(header[3] == 'G');
    stream.close();

    std::filesystem::remove(region);
    std::filesystem::remove(image);
}

TEST_CASE("the CLI fails loudly rather than half-succeeding", "[cli]") {
    SECTION("a missing region file") {
        const CliResult result = runCli("diff /nonexistent/a.mca /nonexistent/b.mca");
        CHECK(result.exitCode == 4);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("cannot open"));
    }

    SECTION("the wrong number of arguments") {
        const CliResult result = runCli("diff only-one.mca");
        CHECK(result.exitCode == 2);
    }

    SECTION("render without an output path") {
        const std::filesystem::path region = writeRegion("stratum-cli-f", false);
        const CliResult result = runCli("render \"" + region.string() + "\"");
        CHECK(result.exitCode == 2);
        std::filesystem::remove(region);
    }

    SECTION("an unknown render mode") {
        const std::filesystem::path region = writeRegion("stratum-cli-g", false);
        // --out is never written: the mode is rejected before any output is
        // opened. It still has to be a path this platform could accept, so it
        // is spelled the same way every other scratch path here is rather
        // than as a POSIX literal.
        const std::filesystem::path out = stratum::test::tempPath("stratum-cli-g-out", ".png");
        const CliResult result = runCli("render \"" + region.string() + "\" --out \"" +
                                        out.string() + "\" --mode nonsense");
        CHECK(result.exitCode == 2);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("heightmap"));
        std::filesystem::remove(region);
    }

    SECTION("generate with nothing to generate from") {
        // It used to be the subcommand that was not implemented yet (exit
        // 3); now it is a usage error naming what is missing.
        const CliResult result = runCli("generate");
        CHECK(result.exitCode == 2);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("version root"));
        CHECK_THAT(result.output, !Catch::Matchers::ContainsSubstring("not implemented"));
    }
}

TEST_CASE("render --pack writes a PNG of a density function", "[cli]") {
    const TempPack pack;
    const std::filesystem::path image = stratum::test::tempPath("stratum-cli-density", ".png");
    std::filesystem::remove(image);

    const CliResult result = runCli("render --pack \"" + pack.path().string() +
                                    "\" --function field --seed 7 --size 16x8 --step 4 --out \"" +
                                    image.string() + "\"");

    CHECK(result.exitCode == 0);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("16x8"));
    // The range is printed because a picture cannot be read back into
    // numbers, and a field that came out constant should be obvious from the
    // console rather than only from squinting at the image.
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("values ranged"));
    CHECK(std::filesystem::exists(image));
    CHECK(std::filesystem::file_size(image) > 0U);

    std::filesystem::remove(image);
}

TEST_CASE("render refuses half-given density options", "[cli]") {
    const TempPack pack;
    const std::filesystem::path image = stratum::test::tempPath("stratum-cli-unused", ".png");

    // --pack without --function: there is no default function, and picking
    // one would be a guess about what was wanted.
    const CliResult noFunction =
        runCli("render --pack \"" + pack.path().string() + "\" --out \"" + image.string() + "\"");
    CHECK(noFunction.exitCode == 2);
    CHECK_THAT(noFunction.output, Catch::Matchers::ContainsSubstring("--function"));

    // --function without --pack: nothing to look the name up in.
    const CliResult noPack = runCli("render --function field --out \"" + image.string() + "\"");
    CHECK(noPack.exitCode == 2);
    CHECK_THAT(noPack.output, Catch::Matchers::ContainsSubstring("--pack"));

    // A name the pack does not define is a usage error naming the name, not
    // an empty image.
    const CliResult unknown = runCli("render --pack \"" + pack.path().string() +
                                     "\" --function nope --out \"" + image.string() + "\"");
    CHECK(unknown.exitCode == 2);
    CHECK_THAT(unknown.output, Catch::Matchers::ContainsSubstring("minecraft:nope"));

    CHECK_FALSE(std::filesystem::exists(image));
}

TEST_CASE("render says which node type it cannot draw", "[cli]") {
    const TempPack pack;
    const std::filesystem::path image = stratum::test::tempPath("stratum-cli-refused", ".png");
    std::filesystem::remove(image);

    const CliResult result = runCli("render --pack \"" + pack.path().string() +
                                    "\" --function blended --out \"" + image.string() + "\"");

    // Exit 4 is "the command failed", distinct from exit 2's "you asked for
    // something malformed" — the request was well formed and cannot be met.
    CHECK(result.exitCode == 4);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:slide"));
    CHECK_FALSE(std::filesystem::exists(image));
}

TEST_CASE("validate exits 0 on a clean pack and 1 on one with warnings", "[cli]") {
    const TempPack pack;

    // The pack carries `blended`, whose slide this build cannot
    // evaluate, so it is not clean — exit 1 means "loads, with caveats",
    // matching diff's use of 1 for "there is something to tell you about".
    const CliResult warned = runCli("validate \"" + pack.path().string() + "\"");
    CHECK(warned.exitCode == 1);
    CHECK_THAT(warned.output, Catch::Matchers::ContainsSubstring("minecraft:slide"));
    CHECK_THAT(warned.output, Catch::Matchers::ContainsSubstring("evaluable: 1 of 2"));

    // --strict is where SPEC §8's open question is handed to the caller
    // rather than answered: same findings, fatal exit.
    const CliResult strict = runCli("validate --strict \"" + pack.path().string() + "\"");
    CHECK(strict.exitCode == 4);

    const TempPack clean(/*withUnevaluable=*/false);
    const CliResult ok = runCli("validate \"" + clean.path().string() + "\"");
    CHECK(ok.exitCode == 0);
    CHECK_THAT(ok.output, Catch::Matchers::ContainsSubstring("no findings"));
}

TEST_CASE("validate exits 4 on a pack that will not resolve", "[cli]") {
    const std::filesystem::path root = stratum::test::tempPath("stratum-cli-broken-pack");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "density_function");
    {
        std::ofstream out(root / "density_function" / "a.json");
        out << R"({"type":"minecraft:abs","argument":"nowhere"})";
    }

    const CliResult result = runCli("validate \"" + root.string() + "\"");
    CHECK(result.exitCode == 4);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:nowhere"));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("validate refuses a directory that is not a pack", "[cli]") {
    const std::filesystem::path root = stratum::test::tempPath("stratum-cli-not-a-pack");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // Exit 4, and the message names every layout it looked for rather than
    // saying only that this was not one.
    const CliResult result = runCli("validate \"" + root.string() + "\"");
    CHECK(result.exitCode == 4);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("pack.mcmeta"));

    const CliResult tooMany = runCli("validate one two");
    CHECK(tooMany.exitCode == 2);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("generate --help prints its usage, and so does the top-level help", "[cli][generate]") {
    const CliResult help = runCli("generate --help");
    CHECK(help.exitCode == 0);
    CHECK_THAT(help.output, Catch::Matchers::ContainsSubstring("--dimension"));
    CHECK_THAT(help.output, Catch::Matchers::ContainsSubstring("overworld amplified large_biomes"));

    const CliResult top = runCli("--help");
    CHECK(top.exitCode == 0);
    CHECK_THAT(top.output, Catch::Matchers::ContainsSubstring("stratum generate <version-root"));
}

TEST_CASE("generate writes the overworld over the column the server stores, in every region its "
          "range touches",
          "[cli][generate]") {
    const GeneratePack pack;
    const TempDir out("stratum-cli-generate-overworld");

    // Four chunks around the origin: four regions, one chunk each, so the
    // floorDiv on the way into a region file is exercised on both signs.
    const CliResult result =
        runCli("generate " + quote(pack.root()) + " --dimension overworld --seed 5 --chunks " +
               "-1,-1:0,0 --out " + quote(out.path()));
    INFO(result.output);
    REQUIRE(result.exitCode == 0);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("generated 4 chunk(s) in 4 "
                                                                 "region(s)"));
    for (const auto& [regionX, regionZ] : {std::pair{-1, -1}, {0, -1}, {-1, 0}, {0, 0}}) {
        CAPTURE(regionX, regionZ);
        const auto region =
            stratum::region::RegionFile::open(regionPath(out.path(), regionX, regionZ));
        CHECK(region.chunkCount() == 1U);
    }

    const stratum::chunk::Chunk chunk = readGenerated(out.path(), -1, -1);
    CHECK(chunk.x() == -1);
    CHECK(chunk.z() == -1);
    CHECK(chunk.status() == "minecraft:full");
    CHECK(chunk.dataVersion() == 4671);
    // The overworld's dimension type: sections -4..19, y -64..319.
    CHECK(chunk.lowestSection() == -4);
    CHECK(chunk.sections().size() == 24U);
    CHECK(chunk.minY() == -64);
    CHECK(chunk.maxY() == 319);

    CHECK(blockAt(chunk, 5, -64, 7) == "minecraft:stone");
    CHECK(blockAt(chunk, 5, -3, 7) == "minecraft:stone");
    // The surface rule ran, and the state it placed kept its property.
    CHECK(blockAt(chunk, 5, -2, 7) == "minecraft:grass_block[snowy=false]");
    CHECK(blockAt(chunk, 5, -1, 7) == "minecraft:grass_block[snowy=false]");
    CHECK(blockAt(chunk, 5, 0, 7) == "minecraft:water");
    CHECK(blockAt(chunk, 5, 6, 7) == "minecraft:water");
    CHECK(blockAt(chunk, 5, 9, 7) == "minecraft:air");
    CHECK(blockAt(chunk, 5, 319, 7) == "minecraft:air");

    // A biome for every cell of every section, bottom to top.
    CHECK(biomeAt(chunk, 0, -64, 0) == "minecraft:plains");
    CHECK(biomeAt(chunk, 12, 100, 4) == "minecraft:plains");
    CHECK(biomeAt(chunk, 15, 319, 15) == "minecraft:plains");

    // The heightmaps: the water's top for the surface and for motion, the
    // grass under it for the ocean floor.
    const auto surface = chunk.heightmap(stratum::chunk::Heightmap::WorldSurface);
    const auto motion = chunk.heightmap(stratum::chunk::Heightmap::MotionBlocking);
    const auto floor = chunk.heightmap(stratum::chunk::Heightmap::OceanFloor);
    REQUIRE(surface.has_value());
    REQUIRE(motion.has_value());
    REQUIRE(floor.has_value());
    REQUIRE(chunk.heightmap(stratum::chunk::Heightmap::MotionBlockingNoLeaves).has_value());
    const std::optional<int> top = (*surface)[0];
    REQUIRE(top.has_value());
    CHECK(blockAt(chunk, 0, *top, 0) == "minecraft:water");
    CHECK(blockAt(chunk, 0, *top + 1, 0) == "minecraft:air");
    CHECK((*motion)[0] == top);
    CHECK((*floor)[0] == std::optional<int>(-1));
    CHECK(chunk.postProcessing().empty());
}

TEST_CASE("generate writes the End's biomes over its whole stored column, past its noise height",
          "[cli][generate]") {
    const GeneratePack pack;
    const TempDir out("stratum-cli-generate-end");

    // Two runs into one directory: different regions, so the second does
    // not trip over the first's file.
    for (const char* chunks : {"0,0", "100,0"}) {
        const CliResult result =
            runCli("generate " + quote(pack.root()) + " --dimension end --seed 5 --chunks " +
                   chunks + " --out " + quote(out.path()));
        INFO(result.output);
        REQUIRE(result.exitCode == 0);
    }

    const stratum::chunk::Chunk island = readGenerated(out.path(), 0, 0);
    // The End's dimension type: sections 0..15, y 0..255 — twice its noise
    // height.
    CHECK(island.lowestSection() == 0);
    CHECK(island.sections().size() == 16U);
    CHECK(island.maxY() == 255);
    CHECK(blockAt(island, 3, 0, 3) == "minecraft:end_stone");
    CHECK(blockAt(island, 3, 31, 3) == "minecraft:end_stone");
    CHECK(blockAt(island, 3, 40, 3) == "minecraft:air");
    CHECK(blockAt(island, 3, 200, 3) == "minecraft:air");
    // On the central island, and stored above the noise height too.
    CHECK(biomeAt(island, 0, 0, 0) == "minecraft:the_end");
    CHECK(biomeAt(island, 0, 252, 0) == "minecraft:the_end");

    // Off the island the router's erosion decides, and 0.5 is highlands.
    const stratum::chunk::Chunk far = readGenerated(out.path(), 100, 0);
    CHECK(biomeAt(far, 8, 64, 8) == "minecraft:end_highlands");
    CHECK(biomeAt(far, 8, 252, 8) == "minecraft:end_highlands");
}

TEST_CASE("generate writes the same bytes whatever --threads says, from a pack or its frozen "
          "pipeline",
          "[cli][generate]") {
    const GeneratePack pack;
    const std::filesystem::path blob = pack.freeze();
    const TempDir one("stratum-cli-generate-one");
    const TempDir three("stratum-cli-generate-three");
    const TempDir frozen("stratum-cli-generate-frozen");

    const std::string request = " --dimension overworld --seed -7 --chunks 0,0:2,1 --out ";
    const CliResult first = runCli("generate " + quote(pack.root()) + request + quote(one.path()));
    const CliResult second =
        runCli("generate " + quote(pack.root()) + request + quote(three.path()) + " --threads 3");
    const CliResult third = runCli("generate " + quote(blob) + request + quote(frozen.path()));
    INFO(first.output << second.output << third.output);
    REQUIRE(first.exitCode == 0);
    REQUIRE(second.exitCode == 0);
    REQUIRE(third.exitCode == 0);

    const std::vector<char> bytes = bytesOf(regionPath(one.path(), 0, 0));
    CHECK(bytes.size() > stratum::region::kHeaderBytes);
    CHECK(bytesOf(regionPath(three.path(), 0, 0)) == bytes);
    CHECK(bytesOf(regionPath(frozen.path(), 0, 0)) == bytes);
    CHECK(stratum::region::RegionFile::open(regionPath(one.path(), 0, 0)).chunkCount() == 6U);
}

TEST_CASE("generate refuses what it cannot do, by name, before writing anything",
          "[cli][generate]") {
    const GeneratePack pack;
    const TempDir out("stratum-cli-generate-refused");
    const std::string root = quote(pack.root());
    const std::string to = " --out " + quote(out.path());

    SECTION("a dimension it has never heard of") {
        const CliResult result =
            runCli("generate " + root + " --dimension mars --seed 1 --chunks 0,0" + to);
        CHECK(result.exitCode == 2);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("unknown dimension 'mars'"));
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("large_biomes"));
    }

    SECTION("a request missing a piece, or saying it twice") {
        CHECK(runCli("generate " + root + " --seed 1 --chunks 0,0" + to).exitCode == 2);
        CHECK(runCli("generate " + root + " --dimension end --chunks 0,0" + to).exitCode == 2);
        CHECK(runCli("generate " + root + " --dimension end --seed 1" + to).exitCode == 2);
        CHECK(runCli("generate " + root + " --dimension end --seed 1 --chunks 0,0").exitCode == 2);
        const CliResult both =
            runCli("generate " + root + " --dimension end --seed 1 --chunks 0,0 --region 0,0" + to);
        CHECK(both.exitCode == 2);
        CHECK_THAT(both.output, Catch::Matchers::ContainsSubstring("exactly one of"));
    }

    SECTION("a malformed range, thread count or seed") {
        const std::string prefix = "generate " + root + " --dimension end ";
        CHECK(runCli(prefix + "--seed 1 --chunks 3,3:2,2" + to).exitCode == 2);
        CHECK(runCli(prefix + "--seed 1 --chunks 3" + to).exitCode == 2);
        CHECK(runCli(prefix + "--seed 1 --region 0,0:-1,0" + to).exitCode == 2);
        CHECK(runCli(prefix + "--seed 1 --chunks 0,0 --threads 0" + to).exitCode == 2);
        CHECK(runCli(prefix + "--seed x --chunks 0,0" + to).exitCode == 2);
        const CliResult far = runCli(prefix + "--seed 1 --chunks 1875000,0" + to);
        CHECK(far.exitCode == 2);
        CHECK_THAT(far.output, Catch::Matchers::ContainsSubstring("world limit"));
        CHECK(runCli(prefix + "--seed 1 --region 58593,0" + to).exitCode == 2);
    }

    SECTION("a dimension the pipeline does not carry") {
        const CliResult result =
            runCli("generate " + root + " --dimension large_biomes --seed 1 --chunks 0,0" + to);
        CHECK(result.exitCode == 4);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:large_biomes"));
    }

    SECTION("a dimension the library refuses") {
        // A router entry this build cannot evaluate: refused by the library,
        // naming the node, and passed on with the dimension's name.
        const CliResult result =
            runCli("generate " + root + " --dimension nether --seed 1 --chunks 0,0" + to);
        CHECK(result.exitCode == 4);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("'nether'"));
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:slide"));
    }

    SECTION("a dimension whose noise height leaves the column it is stored in") {
        const CliResult result =
            runCli("generate " + root + " --dimension amplified --seed 1 --chunks 0,0" + to);
        CHECK(result.exitCode == 4);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("y -80..303"));
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("y -64..319"));
    }

    SECTION("something that is neither a version root nor a blob") {
        const CliResult missing = runCli("generate " + quote(out.path() / "nowhere") +
                                         " --dimension end --seed 1 --chunks 0,0" + to);
        CHECK(missing.exitCode == 4);
        CHECK_THAT(missing.output, Catch::Matchers::ContainsSubstring("neither a version root"));
        const CliResult worldgenless = runCli("generate " + quote(pack.root() / "worldgen") +
                                              " --dimension end --seed 1 --chunks 0,0" + to);
        CHECK(worldgenless.exitCode == 4);
        CHECK_THAT(worldgenless.output, Catch::Matchers::ContainsSubstring("no worldgen/"));
    }

    // Nothing above wrote a region file.
    CHECK_FALSE(std::filesystem::exists(regionPath(out.path(), 0, 0)));
}

TEST_CASE("generate refuses to replace a region file unless --force says to", "[cli][generate]") {
    const GeneratePack pack;
    const TempDir out("stratum-cli-generate-force");
    const std::string request =
        "generate " + quote(pack.root()) + " --dimension end --seed 1 --out " + quote(out.path());

    REQUIRE(runCli(request + " --chunks 0,0:1,0").exitCode == 0);
    const std::filesystem::path region = regionPath(out.path(), 0, 0);
    const std::vector<char> before = bytesOf(region);

    const CliResult refused = runCli(request + " --chunks 5,5");
    CHECK(refused.exitCode == 4);
    CHECK_THAT(refused.output, Catch::Matchers::ContainsSubstring("already exists"));
    CHECK(bytesOf(region) == before);

    // Replaced whole: the chunks outside the new range are gone with it.
    const CliResult forced = runCli(request + " --chunks 5,5 --force");
    CHECK(forced.exitCode == 0);
    const auto replaced = stratum::region::RegionFile::open(region);
    CHECK(replaced.chunkCount() == 1U);
    CHECK(replaced.hasChunk(5, 5));
}
