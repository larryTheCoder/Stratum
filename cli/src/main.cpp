// Stratum — standalone CLI entry point.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The commands are thin: argument handling and reporting live here, while
// everything they actually do lives in lib/, where it is unit-tested without
// spawning a process — except `generate`'s chunk assembly, which lib/ does
// not hold yet and generate.cpp does. Subcommands that are not implemented
// yet say which milestone owns them and exit non-zero — a tool that silently
// succeeds while doing nothing is the failure mode this project treats as
// most severe (SPEC §8).

#include "generate.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/image/png.hpp>
#include <stratum/region/diff.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/render/density_render.hpp>
#include <stratum/render/region_render.hpp>
#include <stratum/validate/pack_report.hpp>
#include <stratum/version.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
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
     .summary = "compare two region files' blocks and stored biomes, in Java space"},
    {.name = "render",
     .milestone = "M1",
     .summary = "draw a region file, or a density function's field, to a PNG"},
    {.name = "validate",
     .milestone = "M2",
     .summary = "load and schema-check a pack without generating"},
    {.name = "generate",
     .milestone = "M5",
     .summary = "generate region files for a seed and chunk range"},
});

void printGenerateUsage(std::ostream& out) {
    out << "  stratum generate <version-root | pipeline.blob> --dimension <name> --seed N\n"
        << "      (--region RX,RZ[:RX,RZ] | --chunks X,Z[:X,Z]) --out <dir>\n"
        << "      [--threads N] [--force]\n"
        << "      Writes Anvil region files, r.<x>.<z>.mca, holding the blocks and\n"
        << "      biomes this build generates (terrain, aquifers, ore veins, biomes,\n"
        << "      surface rules; no carvers, features or structures) in Java block\n"
        << "      space, from a tools/fetch-vanilla version root or a frozen pipeline.\n"
        << "      --dimension names vanilla's pairing of noise settings and biome\n"
        << "      source, stored over its dimension type's column:\n"
        << "       ";
    for (const stratum::cli::DimensionPreset& preset : stratum::cli::dimensionPresets()) {
        out << ' ' << preset.name;
    }
    out << "\n"
        << "      One the library cannot compile is refused, saying why. Ranges\n"
        << "      include both corners; a region the range covers in part holds only\n"
        << "      the chunks asked for. An existing region file is refused unless\n"
        << "      --force. --threads (default 1) does not change a byte of the output.\n";
}

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
        << "      Compares every block and every stored biome. Exits 0 when both are\n"
        << "      identical, 1 when either differs. --max lists up to n of each.\n"
        << "  stratum render <region.mca> --out <file.png> [--mode heightmap|biome|blocks]\n"
        << "  stratum render --pack <dir> --function <id> --out <file.png>\n"
        << "      [--seed N] [--origin X,Z] [--y N] [--step N] [--size WxH]\n"
        << "      [--ramp signed|grey]\n"
        << "      Shades one density function across an area. This is not terrain\n"
        << "      height: terrain needs the cell sampler (M3).\n"
        << "  stratum validate <pack-dir> [--seed N] [--strict]\n"
        << "      Exits 0 when clean, 1 when there are warnings, 4 on errors.\n"
        << "      --strict makes warnings fatal too.\n";
    printGenerateUsage(out);
    out << "\noptions:\n"
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
            options.maxBiomeDifferences = options.maxBlockDifferences;
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
              << " block position(s), " << report.biomesCompared << " biome cell(s)\n";

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

    // Each cell named by its lowest block: one biome covers 4x4x4 of them.
    for (const stratum::region::BiomeDifference& difference : report.biomeDifferences) {
        std::cout << "biome (" << difference.x << ", " << difference.y << ", " << difference.z
                  << "): " << difference.left << " != " << difference.right << '\n';
    }

    if (report.biomeDifferenceCount > report.biomeDifferences.size()) {
        std::cout << "... and " << (report.biomeDifferenceCount - report.biomeDifferences.size())
                  << " more differing biome cell(s); raise --max to see them\n";
    }

    if (report.identical()) {
        std::cout << "regions are identical: every block and every stored biome\n";
        return kExitOk;
    }

    std::cout << report.blockDifferenceCount << " differing block(s), "
              << report.biomeDifferenceCount << " differing biome cell(s), "
              << report.chunkFindings.size() << " chunk-level finding(s)\n";
    return kExitDifferences;
}

[[nodiscard]] bool parseInt32(std::string_view text, std::int32_t& value) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] bool parseInt64(std::string_view text, std::int64_t& value) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

/// "X,Z" — a pair, because two separate options for one coordinate is two
/// chances to give only half of it.
[[nodiscard]] bool parseOrigin(std::string_view text, std::int32_t& x, std::int32_t& z) {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    return parseInt32(text.substr(0, comma), x) && parseInt32(text.substr(comma + 1), z);
}

/// "WxH", or "N" for a square.
[[nodiscard]] bool parseDimensions(std::string_view text, std::size_t& width, std::size_t& height) {
    const std::size_t cross = text.find('x');
    if (cross == std::string_view::npos) {
        if (!parseSize(text, width)) {
            return false;
        }
        height = width;
        return true;
    }
    return parseSize(text.substr(0, cross), width) && parseSize(text.substr(cross + 1), height);
}

struct DensityRenderRequest {
    std::string_view pack;
    std::string_view function;
    std::int64_t seed = 0;
    stratum::render::DensityRenderOptions options;
};

int runDensityRender(const DensityRenderRequest& request, std::string_view output) {
    const stratum::data::Pack pack = stratum::data::Pack::open(std::filesystem::path(request.pack));
    const stratum::density::Graph graph = stratum::density::Graph::resolveAll(pack);

    const auto id = stratum::data::ResourceLocation::parse(request.function);
    if (!graph.contains(id)) {
        std::cerr << "stratum render: the pack defines no density function '" << id.toString()
                  << "'\n";
        return kExitUsage;
    }

    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, graph.referencedNoises(), request.seed,
        // No dimension here, so no `legacy_random_source` to honour:
        // `render --pack` draws one function's field, not a world.
        stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(graph, noises);

    const stratum::render::DensityField field =
        stratum::render::renderDensity(interpreter, graph.rootOf(id), request.options);
    stratum::image::writePng(std::filesystem::path(output), field.image);

    std::cout << "wrote " << output << " (" << field.image.width() << "x" << field.image.height()
              << ", " << id.toString() << ", seed " << request.seed << ", step "
              << request.options.step << ", " << stratum::render::rampName(request.options.ramp)
              << ")\n"
              << "values ranged " << field.minimum << " to " << field.maximum << " over "
              << field.samples << " sample(s)\n";
    if (field.nonFinite > 0) {
        // Said out loud: those pixels are drawn in a colour of their own, and
        // a reader who did not know that would read them as data.
        std::cout << field.nonFinite << " sample(s) were not finite and are drawn in magenta\n";
    }
    return kExitOk;
}

int runRender(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> paths;
    std::string_view output;
    stratum::render::Mode mode = stratum::render::Mode::Heightmap;
    DensityRenderRequest density;

    // One option needs a value and got none: said the same way every time,
    // naming the option rather than the position.
    const auto needsValue = [](std::string_view option) {
        std::cerr << "stratum render: " << option << " needs a value\n";
        return kExitUsage;
    };

    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view argument = args[i];
        if (argument == "--out") {
            if (i + 1 >= args.size()) {
                return needsValue(argument);
            }
            output = args[++i];
        } else if (argument == "--mode") {
            if (i + 1 >= args.size() || !stratum::render::parseMode(args[i + 1], mode)) {
                std::cerr << "stratum render: --mode must be heightmap, biome or blocks\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--pack") {
            if (i + 1 >= args.size()) {
                return needsValue(argument);
            }
            density.pack = args[++i];
        } else if (argument == "--function") {
            if (i + 1 >= args.size()) {
                return needsValue(argument);
            }
            density.function = args[++i];
        } else if (argument == "--seed") {
            if (i + 1 >= args.size() || !parseInt64(args[i + 1], density.seed)) {
                std::cerr << "stratum render: --seed must be a 64-bit integer\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--origin") {
            if (i + 1 >= args.size() ||
                !parseOrigin(args[i + 1], density.options.originX, density.options.originZ)) {
                std::cerr << "stratum render: --origin must be X,Z\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--y") {
            if (i + 1 >= args.size() || !parseInt32(args[i + 1], density.options.y)) {
                std::cerr << "stratum render: --y must be an integer\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--step") {
            if (i + 1 >= args.size() || !parseInt32(args[i + 1], density.options.step)) {
                std::cerr << "stratum render: --step must be an integer\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--size") {
            if (i + 1 >= args.size() ||
                !parseDimensions(args[i + 1], density.options.width, density.options.height)) {
                std::cerr << "stratum render: --size must be WxH, or N for a square\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--ramp") {
            if (i + 1 >= args.size() ||
                !stratum::render::parseRamp(args[i + 1], density.options.ramp)) {
                std::cerr << "stratum render: --ramp must be signed or grey\n";
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

    if (output.empty()) {
        std::cerr << "stratum render: --out <file.png> is required\n\n";
        printUsage(std::cerr);
        return kExitUsage;
    }

    // Two forms share the subcommand, so refuse a mixture rather than
    // quietly honouring one half of what was asked for.
    if (!density.pack.empty()) {
        if (!paths.empty()) {
            std::cerr << "stratum render: --pack renders a density function, so it takes no "
                         "region file (got '"
                      << paths.front() << "')\n";
            return kExitUsage;
        }
        if (density.function.empty()) {
            std::cerr << "stratum render: --pack needs --function <id> to say what to render\n";
            return kExitUsage;
        }
        return runDensityRender(density, output);
    }

    if (!density.function.empty()) {
        std::cerr << "stratum render: --function needs --pack <dir> to look the function up in\n";
        return kExitUsage;
    }

    if (paths.size() != 1) {
        std::cerr << "stratum render: expects one region file, or --pack with --function\n\n";
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

int runValidate(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> paths;
    stratum::validate::ValidateOptions options;
    bool strict = false;

    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view argument = args[i];
        if (argument == "--seed") {
            if (i + 1 >= args.size() || !parseInt64(args[i + 1], options.seed)) {
                std::cerr << "stratum validate: --seed must be a 64-bit integer\n";
                return kExitUsage;
            }
            ++i;
        } else if (argument == "--strict") {
            strict = true;
        } else if (argument.starts_with("-")) {
            std::cerr << "stratum validate: unknown option '" << argument << "'\n";
            return kExitUsage;
        } else {
            paths.push_back(argument);
        }
    }

    if (paths.size() != 1) {
        std::cerr << "stratum validate: expects exactly one pack directory\n\n";
        printUsage(std::cerr);
        return kExitUsage;
    }

    const stratum::data::Pack pack = stratum::data::Pack::open(std::filesystem::path(paths[0]));
    const stratum::validate::Report report = stratum::validate::validatePack(pack, options);

    std::cout << paths[0] << ": "
              << (pack.layout() == stratum::data::Pack::Layout::DataPack
                      ? "data pack"
                      : "extracted worldgen tree")
              << ", " << pack.size() << " entr" << (pack.size() == 1 ? "y" : "ies") << "\n";

    for (const stratum::validate::RegistryCount& registry : report.registries) {
        std::cout << "  " << stratum::data::registryDirectory(registry.registry) << "  "
                  << registry.entries;
        if (!registry.supported) {
            std::cout << "  (not executed)";
        } else if (!registry.interpreted) {
            std::cout << "  (loaded, not yet interpreted)";
        }
        std::cout << '\n';
    }

    if (report.resolved) {
        std::cout << "graph: " << report.nodes << " node(s), " << report.splines << " spline(s), "
                  << report.noisesReferenced << " noise(s) referenced\n"
                  << "evaluable: " << report.evaluable << " of " << report.densityFunctions
                  << " density function(s)";
        if (report.noiseSettings > 0) {
            std::cout << ", " << report.routerEntriesEvaluable << " of " << report.routerEntries
                      << " router entr" << (report.routerEntries == 1 ? "y" : "ies") << " across "
                      << report.dimensionsChecked << " of " << report.noiseSettings
                      << " dimension(s)";
        }
        std::cout << '\n';
    }

    for (const stratum::validate::Finding& finding : report.findings) {
        std::cout << stratum::validate::severityName(finding.severity) << ": ";
        if (!finding.subject.empty()) {
            std::cout << finding.subject << " — ";
        }
        std::cout << finding.message << '\n';
    }

    const std::size_t errors = report.count(stratum::validate::Severity::Error);
    const std::size_t warnings = report.count(stratum::validate::Severity::Warning);

    if (errors > 0) {
        std::cout << errors << " error(s), " << warnings << " warning(s)\n";
        return kExitFailed;
    }
    if (warnings > 0) {
        // --strict is where SPEC §8's open question is put to the caller
        // rather than answered: whether an unexecutable registry should be
        // fatal depends on whose pack it is, and vanilla's own data could
        // not load if this build decided it.
        std::cout << warnings << " warning(s)\n";
        return strict ? kExitFailed : kExitDifferences;
    }
    std::cout << "no findings\n";
    return kExitOk;
}

/// "X,Z" for one corner, or "X,Z:X,Z" for two — both included, the second
/// at or above the first on each axis. A range given backwards is refused
/// rather than turned round: it is likelier a typo than a request.
[[nodiscard]] bool parseCorners(std::string_view text, std::int32_t& minX, std::int32_t& minZ,
                                std::int32_t& maxX, std::int32_t& maxZ) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        if (!parseOrigin(text, minX, minZ)) {
            return false;
        }
        maxX = minX;
        maxZ = minZ;
        return true;
    }
    return parseOrigin(text.substr(0, colon), minX, minZ) &&
           parseOrigin(text.substr(colon + 1), maxX, maxZ) && minX <= maxX && minZ <= maxZ;
}

/// Milliseconds per chunk, to one decimal place.
[[nodiscard]] std::string perChunk(std::chrono::nanoseconds total, std::size_t chunks) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(1)
         << (std::chrono::duration<double, std::milli>(total).count() / static_cast<double>(chunks))
         << " ms";
    return text.str();
}

int runGenerate(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> paths;
    std::string_view dimensionName;
    std::string_view output;
    std::string_view regionText;
    std::string_view chunkText;
    bool seedGiven = false;
    stratum::cli::GenerateRequest request;

    const auto needsValue = [](std::string_view option) {
        std::cerr << "stratum generate: " << option << " needs a value\n";
        return kExitUsage;
    };

    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view argument = args[i];
        if (argument == "-h" || argument == "--help") {
            printGenerateUsage(std::cout);
            return kExitOk;
        }
        if (argument == "--force") {
            request.force = true;
            continue;
        }
        if (!argument.starts_with("-")) {
            paths.push_back(argument);
            continue;
        }
        if (argument != "--dimension" && argument != "--seed" && argument != "--region" &&
            argument != "--chunks" && argument != "--out" && argument != "--threads") {
            std::cerr << "stratum generate: unknown option '" << argument << "'\n";
            return kExitUsage;
        }
        if (i + 1 >= args.size()) {
            return needsValue(argument);
        }
        const std::string_view value = args[++i];
        if (argument == "--dimension") {
            dimensionName = value;
        } else if (argument == "--seed") {
            if (!parseInt64(value, request.seed)) {
                std::cerr << "stratum generate: --seed must be a 64-bit integer\n";
                return kExitUsage;
            }
            seedGiven = true;
        } else if (argument == "--region") {
            regionText = value;
        } else if (argument == "--chunks") {
            chunkText = value;
        } else if (argument == "--out") {
            output = value;
        } else {
            std::size_t threads = 0;
            constexpr std::size_t kMaxThreads = 256;
            if (!parseSize(value, threads) || threads == 0 || threads > kMaxThreads) {
                std::cerr << "stratum generate: --threads must be 1 to " << kMaxThreads << '\n';
                return kExitUsage;
            }
            request.threads = static_cast<unsigned>(threads);
        }
    }

    // Each missing piece by name: none of them has a default worth guessing.
    if (paths.size() != 1) {
        std::cerr << "stratum generate: expects one version root or pipeline blob, got "
                  << paths.size() << "\n\n";
        printGenerateUsage(std::cerr);
        return kExitUsage;
    }
    if (dimensionName.empty()) {
        std::cerr << "stratum generate: --dimension <name> is required\n";
        return kExitUsage;
    }
    request.dimension = stratum::cli::findDimensionPreset(dimensionName);
    if (request.dimension == nullptr) {
        std::cerr << "stratum generate: unknown dimension '" << dimensionName
                  << "'; --dimension is one of:";
        for (const stratum::cli::DimensionPreset& preset : stratum::cli::dimensionPresets()) {
            std::cerr << ' ' << preset.name;
        }
        std::cerr << '\n';
        return kExitUsage;
    }
    if (!seedGiven) {
        std::cerr << "stratum generate: --seed N is required\n";
        return kExitUsage;
    }
    if (output.empty()) {
        std::cerr << "stratum generate: --out <dir> is required\n";
        return kExitUsage;
    }
    if (regionText.empty() == chunkText.empty()) {
        std::cerr << "stratum generate: give exactly one of --region and --chunks\n";
        return kExitUsage;
    }

    stratum::cli::ChunkRange& range = request.chunks;
    if (!chunkText.empty()) {
        if (!parseCorners(chunkText, range.minX, range.minZ, range.maxX, range.maxZ)) {
            std::cerr << "stratum generate: --chunks must be X,Z or X,Z:X,Z, the second "
                         "corner at or above the first\n";
            return kExitUsage;
        }
    } else {
        std::int32_t minX = 0;
        std::int32_t minZ = 0;
        std::int32_t maxX = 0;
        std::int32_t maxZ = 0;
        constexpr std::int64_t kRegionChunks = stratum::region::kChunksPerAxis;
        // Regions whose chunks would leave the world limit are refused
        // here, in 64 bits, before any multiplication can overflow.
        constexpr std::int64_t kRegionLimit =
            (std::int64_t{stratum::cli::kMaxChunkCoordinate} + 1) / kRegionChunks;
        if (!parseCorners(regionText, minX, minZ, maxX, maxZ) || minX < -kRegionLimit ||
            minZ < -kRegionLimit || maxX >= kRegionLimit || maxZ >= kRegionLimit) {
            std::cerr << "stratum generate: --region must be RX,RZ or RX,RZ:RX,RZ, the second "
                         "corner at or above the first, each within "
                      << -kRegionLimit << ".." << (kRegionLimit - 1) << '\n';
            return kExitUsage;
        }
        range.minX = static_cast<std::int32_t>(minX * kRegionChunks);
        range.minZ = static_cast<std::int32_t>(minZ * kRegionChunks);
        range.maxX = static_cast<std::int32_t>((maxX * kRegionChunks) + kRegionChunks - 1);
        range.maxZ = static_cast<std::int32_t>((maxZ * kRegionChunks) + kRegionChunks - 1);
    }
    if (range.minX < stratum::cli::kMinChunkCoordinate ||
        range.minZ < stratum::cli::kMinChunkCoordinate ||
        range.maxX > stratum::cli::kMaxChunkCoordinate ||
        range.maxZ > stratum::cli::kMaxChunkCoordinate) {
        std::cerr << "stratum generate: chunks must lie within the 30 000 000-block world "
                     "limit, chunk coordinates "
                  << stratum::cli::kMinChunkCoordinate << ".." << stratum::cli::kMaxChunkCoordinate
                  << '\n';
        return kExitUsage;
    }
    request.source = std::filesystem::path(paths.front());
    request.out = std::filesystem::path(output);

    const stratum::cli::DimensionPreset& preset = *request.dimension;
    std::cout << "generating " << preset.name << " (noise settings " << preset.noiseSettings
              << ", biome source " << preset.biomeSource << "), seed " << request.seed
              << ", chunks (" << range.minX << ", " << range.minZ << ") to (" << range.maxX << ", "
              << range.maxZ << "): " << range.count() << " chunk(s)\n";

    const stratum::cli::GenerateReport report =
        stratum::cli::generate(request, [](const std::filesystem::path& path, std::size_t chunks) {
            std::cout << "wrote " << path.string() << " (" << chunks << " chunk(s))\n";
        });

    // Per chunk on ONE generation thread, whatever --threads said: the
    // stage times are summed over threads. The rate is wall clock.
    const double seconds = std::chrono::duration<double>(report.wall).count();
    std::cout << "generated " << report.chunks << " chunk(s) in " << report.regions
              << " region(s) in " << std::fixed << std::setprecision(1) << seconds << " s on "
              << request.threads << " thread(s)";
    if (seconds > 0.0) {
        std::cout << ", " << (static_cast<double>(report.chunks) / seconds) << " chunk(s)/s";
    }
    std::cout << "\nper chunk on one thread: fill " << perChunk(report.stages.fill, report.chunks)
              << ", biomes " << perChunk(report.stages.biomes, report.chunks) << ", encode "
              << perChunk(report.stages.encode, report.chunks) << "; region writes "
              << perChunk(report.stages.write, report.chunks) << '\n';
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
    if (command == "validate") {
        return runValidate(args);
    }
    if (command == "generate") {
        return runGenerate(args);
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
