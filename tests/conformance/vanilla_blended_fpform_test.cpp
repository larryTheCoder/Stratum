// Stratum — old_blended_noise's floating-point form, against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The golden confirmation (IMPLEMENTER-RULING-01 §2, blended noise P2) of the
// floating-point amendments in spec/blended-noise-spec.md (Q1.4's fp-form,
// Q1.5, Q2.1). They state an evaluation order that differs from the one this
// build carried in two places that move the last bit:
//
//   A  the main sampler's position, ((x * M) / F) * s rather than
//      (x * (M / F)) * s — about 9% of block positions;
//   S  the selector, (main / 10 + 1) / 2 rather than 0.5 + 0.05 * main —
//      about 1.3%.
//
// (The cap and divisor forms, a third, move no value at all over the probe
// window: tools/analysis/blended-fpform-check.cpp.) A block reading cannot see
// one ulp, so tools/analysis/blended-fpform-probe.sh bisects: at each point
// below, each threshold T is its own dimension whose final density is
// range_choice(base_3d_noise, [-1000, T) -> -1, else 1), so the block AT the
// point is open (air, or water below sea level) exactly when the server's
// value is below T. The thresholds are
// every distinct candidate of the eight A/C/S combinations plus one ulp
// above the largest, so the readings pin the server's value to one
// candidate. This requires this build's value to fall on the server's side
// of every threshold, and names the candidate the server computed.
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

#include <stratum/noise/blended.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char* kScript = "tools/analysis/blended-fpform-probe.sh";
constexpr std::int64_t kSeed = 42;

/// One bisected point: its eight candidates (bit 0 the spec's position form,
/// bit 1 its cap and divisor, bit 2 its selector; candidate 0 the form this
/// build carried before, 7 the amendment's) and the thresholds probed there,
/// in the probe's order (blended_fpform_targets.inc says where they came
/// from).
struct Target {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
    std::array<double, 8> candidates;
    std::vector<double> thresholds;
};

const std::vector<Target> kTargets{
#include "blended_fpform_targets.inc"
};

/// Bit equality: the comparison the thresholds make.
[[nodiscard]] bool same(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::filesystem::path probeDir() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" / "blendedfp_s42";
}

/// The probe's own dimension name for threshold @p j of target @p i.
[[nodiscard]] std::string dimensionName(std::size_t i, const Target& t, std::size_t j) {
    std::string name = "p" + std::to_string(i) + "_" + std::to_string(t.x) + "_" +
                       std::to_string(t.y) + "_" + std::to_string(t.z) + "_" + std::to_string(j);
    for (char& c : name) {
        if (c == '-') {
            c = 'm';
        }
    }
    return name;
}

} // namespace

TEST_CASE("old_blended_noise is evaluated in the server's floating-point form",
          "[conformance][blended]") {
    if (!std::filesystem::is_regular_file(probeDir() / "spec.json")) {
        SKIP("no blended-noise bisection at " << probeDir() << "; generate it with " << kScript
                                              << " --accept-eula, or tools/probe-worlds "
                                                 "generate --only blendedfp_s42 --accept-eula");
    }
    stratum::test::requireFrozen(probeDir(), kScript);
    stratum::test::requireSeed(probeDir(), kSeed);

    const auto noise = stratum::noise::BlendedNoise::modern(kSeed, {.xzScale = 0.25,
                                                                    .yScale = 0.125,
                                                                    .xzFactor = 80.0,
                                                                    .yFactor = 160.0,
                                                                    .smearScaleMultiplier = 8.0});

    std::array<int, 9> serverCandidate{}; // index 8: none of them
    for (std::size_t i = 0; i < kTargets.size(); ++i) {
        const Target& t = kTargets[i];
        const double ours = noise.sample(t.x, t.y, t.z);
        INFO("point (" << t.x << ", " << t.y << ", " << t.z << "), ours " << ours);
        // The server's value lies in [low, high): the largest threshold it
        // is not below, and the smallest it is.
        double low = -1000.0;
        double high = 1000.0;
        for (std::size_t j = 0; j < t.thresholds.size(); ++j) {
            const std::filesystem::path region = probeDir() / dimensionName(i, t, j) / "r.0.0.mca";
            REQUIRE(std::filesystem::is_regular_file(region));
            stratum::test::GoldenRegion golden(region, 0, 0);
            const auto* block = golden.blockAt(t.x, t.y, t.z);
            REQUIRE(block != nullptr);
            // Below the threshold is open: air, or water under the probe's
            // sea level 63. At or above it is the default block, stone.
            const bool serverBelow = block->name != "minecraft:stone";
            INFO("threshold " << t.thresholds[j] << ": server " << block->name);
            CHECK(serverBelow == (ours < t.thresholds[j]));
            if (serverBelow) {
                high = std::min(high, t.thresholds[j]);
            } else {
                low = std::max(low, t.thresholds[j]);
            }
        }
        // Which candidates the readings leave: the server's value equals a
        // candidate exactly when the candidate is the largest threshold it
        // reached (every distinct candidate is a threshold).
        int matched = 8;
        for (int k = 7; k >= 0; --k) {
            if (same(t.candidates[static_cast<std::size_t>(k)], low)) {
                matched = k;
                break;
            }
        }
        ++serverCandidate[static_cast<std::size_t>(matched)];
        INFO("server in [" << low << ", " << high << ")");
        CHECK(matched != 8);
        // The amendment's form, all three choices: candidate 7 or one
        // equal to it at this point.
        CHECK(same(t.candidates[7], low));
    }
    std::string tally;
    for (const int count : serverCandidate) {
        tally += " " + std::to_string(count);
    }
    INFO("highest candidate the server's value equals, per point (0..7, none):" << tally);
    CHECK(serverCandidate[8] == 0);
}
