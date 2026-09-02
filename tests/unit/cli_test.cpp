// Stratum — CLI end-to-end tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The commands are thin, but their contract — which exit code means what — is
// what scripts and CI depend on, so it is tested against the real binary
// rather than the functions behind it.

#include "support/region_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

using stratum::test::RegionBuilder;
using stratum::test::SectionSpec;

struct CliResult {
    int exitCode = 0;
    std::string output;
};

[[nodiscard]] CliResult runCli(const std::string& arguments) {
    const std::filesystem::path outputPath =
        std::filesystem::temp_directory_path() / "stratum-cli-test-output.txt";
    std::string command = std::string("\"") + STRATUM_CLI_PATH + "\" " + arguments + " > \"" +
                          outputPath.string() + "\" 2>&1";

#ifdef _WIN32
    // std::system runs `cmd /c <command>`, and cmd strips the outer quotes
    // when a command line both begins with a quote and contains more of
    // them, so the executable is never found. Wrapping the whole thing in
    // one more pair is the documented way round that.
    command = "\"" + command + "\"";
#endif

    // Not const: Apple's WIFEXITED and WEXITSTATUS expand to *(int *)&(x),
    // which drops the qualifier off a const int and trips -Wcast-qual.
    int status = std::system(command.c_str());

    std::string output;
    {
        // Scoped so the handle is closed before the file is removed: Windows
        // refuses to delete a file that is still open. Read by size rather
        // than through istreambuf_iterator, which GCC 13's optimiser cannot
        // prove non-null through and reports as a potential null dereference
        // inside <streambuf>.
        std::ifstream stream(outputPath, std::ios::binary);
        if (stream) {
            stream.seekg(0, std::ios::end);
            output.resize(static_cast<std::size_t>(stream.tellg()));
            stream.seekg(0, std::ios::beg);
            stream.read(output.data(), static_cast<std::streamsize>(output.size()));
            output.resize(static_cast<std::size_t>(stream.gcount()));
        }
    }
    std::filesystem::remove(outputPath);

#ifdef _WIN32
    return CliResult{status, output};
#else
    return CliResult{WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
#endif
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

[[nodiscard]] std::filesystem::path writeRegion(const std::string& name, bool withDirt) {
    RegionBuilder builder;
    builder.addChunk(1, 1, {stoneSection(withDirt)});
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    builder.writeTo(path);
    return path;
}

/// A minimal worldgen tree on disk, so the density render path can be driven
/// through the real binary without the vanilla fixtures.
class TempPack {
public:
    explicit TempPack(bool withUnevaluable = true)
        : path_(std::filesystem::temp_directory_path() /
                (withUnevaluable ? "stratum-cli-pack" : "stratum-cli-pack-clean")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
        write("noise", "test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
        write("density_function", "field",
              R"({"type":"minecraft:noise","noise":"test","xz_scale":1.0,"y_scale":0.0})");
        if (withUnevaluable) {
            write("density_function", "blended",
                  R"({"type":"minecraft:old_blended_noise","xz_scale":1.0,"y_scale":1.0,
                      "xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
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

} // namespace

TEST_CASE("diff exits 0 and says so when regions are identical", "[cli]") {
    const std::filesystem::path left = writeRegion("stratum-cli-a.mca", false);
    const std::filesystem::path right = writeRegion("stratum-cli-b.mca", false);

    const CliResult result = runCli("diff \"" + left.string() + "\" \"" + right.string() + "\"");
    CHECK(result.exitCode == 0);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("identical"));

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("diff exits 1 and names the block when regions differ", "[cli]") {
    const std::filesystem::path left = writeRegion("stratum-cli-c.mca", false);
    const std::filesystem::path right = writeRegion("stratum-cli-d.mca", true);

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

TEST_CASE("render writes a PNG", "[cli]") {
    const std::filesystem::path region = writeRegion("stratum-cli-e.mca", false);
    const std::filesystem::path image =
        std::filesystem::temp_directory_path() / "stratum-cli-render.png";

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
        const std::filesystem::path region = writeRegion("stratum-cli-f.mca", false);
        const CliResult result = runCli("render \"" + region.string() + "\"");
        CHECK(result.exitCode == 2);
        std::filesystem::remove(region);
    }

    SECTION("an unknown render mode") {
        const std::filesystem::path region = writeRegion("stratum-cli-g.mca", false);
        const CliResult result =
            runCli("render \"" + region.string() + "\" --out /tmp/x.png --mode nonsense");
        CHECK(result.exitCode == 2);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("heightmap"));
        std::filesystem::remove(region);
    }

    SECTION("a subcommand that is not implemented yet") {
        const CliResult result = runCli("generate");
        CHECK(result.exitCode == 3);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("not implemented"));
    }
}

TEST_CASE("render --pack writes a PNG of a density function", "[cli]") {
    const TempPack pack;
    const std::filesystem::path image =
        std::filesystem::temp_directory_path() / "stratum-cli-density.png";
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
    const std::filesystem::path image =
        std::filesystem::temp_directory_path() / "stratum-cli-unused.png";

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
    const std::filesystem::path image =
        std::filesystem::temp_directory_path() / "stratum-cli-refused.png";
    std::filesystem::remove(image);

    const CliResult result = runCli("render --pack \"" + pack.path().string() +
                                    "\" --function blended --out \"" + image.string() + "\"");

    // Exit 4 is "the command failed", distinct from exit 2's "you asked for
    // something malformed" — the request was well formed and cannot be met.
    CHECK(result.exitCode == 4);
    CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("minecraft:old_blended_noise"));
    CHECK_FALSE(std::filesystem::exists(image));
}

TEST_CASE("validate exits 0 on a clean pack and 1 on one with warnings", "[cli]") {
    const TempPack pack;

    // The pack carries `blended`, whose old_blended_noise this build cannot
    // evaluate, so it is not clean — exit 1 means "loads, with caveats",
    // matching diff's use of 1 for "there is something to tell you about".
    const CliResult warned = runCli("validate \"" + pack.path().string() + "\"");
    CHECK(warned.exitCode == 1);
    CHECK_THAT(warned.output, Catch::Matchers::ContainsSubstring("minecraft:old_blended_noise"));
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
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "stratum-cli-broken-pack";
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
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "stratum-cli-not-a-pack";
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
