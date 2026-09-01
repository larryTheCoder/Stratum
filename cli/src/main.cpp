// Stratum — standalone CLI entry point.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Milestone M0 wires the dispatcher only. Every subcommand below is
// unimplemented and says so on stderr with the milestone that owns it, then
// exits non-zero: a tool that silently succeeds while doing nothing is the
// failure mode this project treats as most severe (SPEC §8).

#include <stratum/version.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitNotImplemented = 3;
constexpr int kExitInternalError = 70;

constexpr std::size_t kSubcommandColumn = 10U;

struct Subcommand {
    std::string_view name;
    std::string_view milestone;
    std::string_view summary;
};

constexpr auto kSubcommands = std::to_array<Subcommand>({
    {.name = "render",
     .milestone = "M1",
     .summary = "render a heightmap / biome / slice image of a pipeline"},
    {.name = "diff",
     .milestone = "M1",
     .summary = "diff engine output against vanilla region files (conformance)"},
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
    out << "\noptions:\n"
        << "  -h, --help       show this message\n"
        << "  -V, --version    print version, engine version and schema pin\n";
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
        std::cerr << "stratum: internal error: " << error.what() << '\n';
        return kExitInternalError;
    } catch (...) {
        std::cerr << "stratum: internal error: unknown exception\n";
        return kExitInternalError;
    }
}
