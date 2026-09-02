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

    const int status = std::system(command.c_str());

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
