// Stratum — reads back tools/analysis/aquifer-ties-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   stratum_aquifer_ties_analyze CORPUS [--stride N] [--only DIM] [--sea-rows FROM TO]
//   stratum_aquifer_ties_analyze --model SPEC SEED [--stride N] [--only DIM]
//
// Scores every reading in aquifer-ties-rivals.hpp — the library's, the
// near-surface exemption on the aborting sample, the clean-room spec's
// first-match Q5.3, and first match on each case's sources alone — through
// the substance decision the filler runs, on the untouched chunks of every
// aquifer dimension of a corpus (default stride 2: every other column both
// ways), the way tests/conformance/vanilla_aquifer_ties_test.cpp does at its
// own stride. --model replays a spec with no server in it: what each reading
// would change against the library's, which is how a design is checked
// before a server runs. Prints, per dimension: the sources ranked anywhere
// in the scored blocks by case, "drift" (a rebuilt scan or first match on a
// source in no case disagreeing with the library — must stay 0), and per
// reading the blocks where it differs from the library and, with a corpus,
// the blocks and marks it gets wrong. Nothing here asserts; the conformance
// case does.
#include "aquifer-ties-rivals.hpp"
#include "aquifer-ties-score.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ties = stratum::analysis::ties;

/// The fetched worldgen tree, for vanilla's `aquifer_barrier` noise:
/// $STRATUM_FIXTURES_DIR as the other aquifer analyzers read it.
[[nodiscard]] std::filesystem::path worldgen() {
    const char* fixtures = std::getenv("STRATUM_FIXTURES_DIR");
    return std::filesystem::path{fixtures != nullptr ? fixtures : ".fixtures"} / "1.21.11" /
           "worldgen";
}

int run(const std::vector<std::string>& args) {
    std::optional<std::string> spec;
    std::optional<std::string> corpus;
    std::int64_t seed = 0;
    std::int32_t stride = 2;
    std::optional<std::pair<std::int32_t, std::int32_t>> seaRows;
    std::optional<std::string> only;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--model" && i + 2 < args.size()) {
            spec = args[i + 1];
            seed = std::stoll(args[i + 2]);
            i += 2;
        } else if (arg == "--stride" && i + 1 < args.size()) {
            stride = std::stoi(args[++i]);
        } else if (arg == "--only" && i + 1 < args.size()) {
            only = args[++i];
        } else if (arg == "--sea-rows" && i + 2 < args.size()) {
            seaRows = std::make_pair(std::stoi(args[i + 1]), std::stoi(args[i + 2]));
            i += 2;
        } else if (!corpus.has_value() && !arg.starts_with("--")) {
            corpus = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (spec.has_value() == corpus.has_value() || stride < 1 || stride > 16) {
        std::fprintf(stderr, "usage: stratum_aquifer_ties_analyze CORPUS | --model SPEC SEED "
                             "[--stride N] [--only DIM]\n");
        return 2;
    }
    std::filesystem::path specPath;
    if (corpus.has_value()) {
        specPath = std::filesystem::path{*corpus} / "spec.json";
        const nlohmann::json manifest =
            nlohmann::json::parse(std::ifstream(std::filesystem::path{*corpus} / "manifest.json"));
        seed = manifest.at("seed").get<std::int64_t>();
    } else {
        specPath = *spec;
    }
    const nlohmann::json entries = nlohmann::json::parse(std::ifstream(specPath));
    for (const auto& entry : entries) {
        const std::string name = entry.at("name").get<std::string>();
        if (!entry.contains("router") || (only.has_value() && *only != name)) {
            continue;
        }
        const ties::Dimension dimension(entry, ties::probeNoise(), worldgen(), seed);
        std::optional<std::filesystem::path> region;
        if (corpus.has_value()) {
            region = std::filesystem::path{*corpus} / name / "r.0.0.mca";
        }
        const std::int32_t sea = dimension.settings().seaLevel;
        const ties::Score score =
            seaRows.has_value() ? ties::scoreReadings(dimension, region, stride,
                                                      sea + seaRows->first, sea + seaRows->second)
                                : ties::scoreReadings(dimension, region, stride);
        std::printf("%s (seed %lld, sea %d): %d chunks, %lld blocks, moved %lld (unexplained "
                    "%lld), marks %lld, drift %lld\n",
                    name.c_str(), static_cast<long long>(seed), dimension.settings().seaLevel,
                    score.chunks, score.blocks, score.moved, score.unexplained, score.marks,
                    score.drift);
        std::printf("  sources:");
        for (std::size_t c = 0; c < score.sources.size(); ++c) {
            std::printf(" %s %lld", ties::kCaseNames.at(c), score.sources.at(c));
        }
        std::printf("\n");
        for (std::size_t r = 0; r < ties::kReadingCount; ++r) {
            std::printf("  %-52s differs %8lld", ties::kReadingNames.at(r),
                        score.differsFromShipped.at(r));
            if (corpus.has_value()) {
                std::printf("  wrong %8lld  marks wrong %6lld", score.wrongBlocks.at(r),
                            score.wrongMarks.at(r));
            }
            std::printf("\n");
        }
        std::fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
