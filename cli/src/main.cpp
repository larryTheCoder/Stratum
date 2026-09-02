// Stratum — standalone CLI entry point.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The commands are thin: argument handling and reporting live here, while
// everything they actually do lives in lib/, where it is unit-tested without
// spawning a process. Subcommands that are not implemented yet say which
// milestone owns them and exit non-zero — a tool that silently succeeds while
// doing nothing is the failure mode this project treats as most severe
// (SPEC §8).

#include <stratum/image/png.hpp>
#include <stratum/region/diff.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/render/region_render.hpp>
#include <stratum/version.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitDifferences = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotImplemented = 3;
constexpr int kExitFailed = 4;
constexpr int kExitInternalError = 70;

constexpr std::size_t kSubcommandColumn = 10U;

struct Subcommand {
    std::string_view name;
    std::string_view milestone;
    std::string_view summary;
};

constexpr auto kSubcommands = std::to_array<Subcommand>({
    {.name = "diff",
     .milestone = "M1",
     .summary = "compare two region files block-for-block in Java block space"},
    {.name = "render", .milestone = "M1", .summary = "render a region file to a PNG"},
    {.name = "validate",
     .milestone = "M2",
     .summary = "load and schema-check a pack without generating"},
    {.name = "generate",
     .milestone = "M3",
     .summary = "generate region files for a seed and chunk range"},
});

void printUsage(std::ostream& out) {
    out << stratum::versionBanner() << "\n\n"
        << "usage: stratum <subcommand> [options]\n\n"
        << "subcommands:\n";
    for (const Subcommand& cmd : kSubcommands) {
        out << "  " << cmd.name;
        for (std::size_t pad = cmd.name.size(); pad < kSubcommandColumn; ++pad) {
            out << ' ';
        }
        out << cmd.summary << "  [" << cmd.milestone << "]\n";
    }
    out << "\n"
        << "  stratum diff <left.mca> <right.mca> [--max <n>]\n"
        << "      Exits 0 when identical, 1 when they differ.\n"
        << "  stratum render <region.mca> --out <file.png> [--mode heightmap|biome|blocks]\n"
        << "\noptions:\n"
        << "  -h, --help       show this message\n"
        << "  -V, --version    print version, engine version and schema pin\n";
}

[[nodiscard]] bool parseSize(std::string_view text, std::size_t& value) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

int runDiff(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> paths;
    stratum::region::DiffOptions options;

    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view argument = args[i];
        if (argument == "--max") {
            if (i + 1 >= args.size() || !parseSize(args[i + 1], options.maxBlockDifferences)) {
                std::cerr << "stratum diff: --max needs a non-negative number\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument.starts_with("-")) {
            std::cerr << "stratum diff: unknown option '" << argument << "'\n";
            return kExitUsage;
        } else {
            paths.push_back(argument);
        }
    }

    if (paths.size() != 2) {
        std::cerr << "stratum diff: expects exactly two region files, got " << paths.size()
                  << "\n\n";
        printUsage(std::cerr);
        return kExitUsage;
    }

    const stratum::region::RegionFile left =
        stratum::region::RegionFile::open(std::filesystem::path(paths[0]));
    const stratum::region::RegionFile right =
        stratum::region::RegionFile::open(std::filesystem::path(paths[1]));

    const stratum::region::DiffReport report = stratum::region::diff(left, right, options);

    std::cout << "compared " << report.chunksCompared << " chunk(s), " << report.blocksCompared
              << " block position(s)\n";

    for (const stratum::region::ChunkFinding& finding : report.chunkFindings) {
        std::cout << "chunk (" << finding.chunkX << ", " << finding.chunkZ
                  << "): " << stratum::region::chunkIssueName(finding.issue);
        if (!finding.detail.empty()) {
            std::cout << " — " << finding.detail;
        }
        std::cout << '\n';
    }

    for (const stratum::region::BlockDifference& difference : report.blockDifferences) {
        std::cout << "block (" << difference.x << ", " << difference.y << ", " << difference.z
                  << "): " << difference.left << " != " << difference.right << '\n';
    }

    if (report.blockDifferenceCount > report.blockDifferences.size()) {
        std::cout << "... and " << (report.blockDifferenceCount - report.blockDifferences.size())
                  << " more differing block(s); raise --max to see them\n";
    }

    if (report.identical()) {
        std::cout << "regions are identical\n";
        return kExitOk;
    }

    std::cout << report.blockDifferenceCount << " differing block(s), "
              << report.chunkFindings.size() << " chunk-level finding(s)\n";
    return kExitDifferences;
}

int runRender(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> paths;
    std::string_view output;
    stratum::render::Mode mode = stratum::render::Mode::Heightmap;

    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view argument = args[i];
        if (argument == "--out") {
            if (i + 1 >= args.size()) {
                std::cerr << "stratum render: --out needs a path\n";
                return kExitUsage;
            }
            output = args[++i];
        } else if (argument == "--mode") {
            if (i + 1 >= args.size() || !stratum::render::parseMode(args[i + 1], mode)) {
                std::cerr << "stratum render: --mode must be heightmap, biome or blocks\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument.starts_with("-")) {
            std::cerr << "stratum render: unknown option '" << argument << "'\n";
            return kExitUsage;
        } else {
            paths.push_back(argument);
        }
    }

    if (paths.size() != 1 || output.empty()) {
        std::cerr << "stratum render: expects one region file and --out <file.png>\n\n";
        printUsage(std::cerr);
        return kExitUsage;
    }

    const stratum::region::RegionFile region =
        stratum::region::RegionFile::open(std::filesystem::path(paths[0]));
    const stratum::image::Image image = stratum::render::renderRegion(region, mode);
    stratum::image::writePng(std::filesystem::path(output), image);

    std::cout << "wrote " << output << " (" << image.width() << "x" << image.height() << ", "
              << stratum::render::modeName(mode) << ", " << region.chunkCount() << " chunk(s))\n";
    return kExitOk;
}

int run(const std::vector<std::string_view>& args) {
    if (args.size() < 2U) {
        printUsage(std::cerr);
        return kExitUsage;
    }

    const std::string_view command = args[1];

    if (command == "-h" || command == "--help" || command == "help") {
        printUsage(std::cout);
        return kExitOk;
    }
    if (command == "-V" || command == "--version" || command == "version") {
        std::cout << stratum::versionBanner() << '\n';
        return kExitOk;
    }
    if (command == "diff") {
        return runDiff(args);
    }
    if (command == "render") {
        return runRender(args);
    }

    for (const Subcommand& cmd : kSubcommands) {
        if (command == cmd.name) {
            std::cerr << "stratum " << cmd.name << ": not implemented — this "
                      << "subcommand is Milestone " << cmd.milestone << " (SPEC §10).\n";
            return kExitNotImplemented;
        }
    }

    std::cerr << "stratum: unknown subcommand '" << command << "'\n\n";
    printUsage(std::cerr);
    return kExitUsage;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv, argv + static_cast<std::size_t>(argc));
        return run(args);
    } catch (const std::exception& error) {
        // Region and NBT problems arrive here: report them plainly and fail,
        // rather than printing a partial result that looks like agreement.
        std::cerr << "stratum: " << error.what() << '\n';
        return kExitFailed;
    } catch (...) {
        std::cerr << "stratum: internal error: unknown exception\n";
        return kExitInternalError;
    }
}
