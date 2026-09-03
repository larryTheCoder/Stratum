// Stratum — the same samples as blended-probe.mjs, from this build.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Deliberately not a `stratum` subcommand: this is a measuring instrument for
// an unsettled question, not a feature, and it should not outlive the
// question. tools/analysis/run-blended-probe.sh compiles it against the built
// library. Keep the sampling grids identical to the .mjs beside it — the whole
// comparison is that the two are asked for the same points.
#include <stratum/noise/blended.hpp>
#include <stratum/rng/java_random.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    using namespace stratum;
    const std::string mode = argc > 1 ? argv[1] : "lines";
    const std::string config = argc > 2 ? argv[2] : "overworld";
    const std::int64_t seed = argc > 3 ? std::atoll(argv[3]) : 0;

    noise::BlendedNoise::Parameters parameters{
        .xzScale = 0.25, .yScale = 0.125, .xzFactor = 80.0, .yFactor = 160.0,
        .smearScaleMultiplier = config == "smear1" ? 1.0 : 8.0,
    };
    // `columns` takes a multiplier rather than a named config, to sweep it.
    if (mode == "columns" || mode == "fine") {
        parameters.smearScaleMultiplier = std::atof(config.c_str());
    }

    // The seeding a modern dimension uses is the open question this cannot
    // answer, so it uses the legacy one. Every statistic the comparison
    // script computes is seed-independent, which is what makes that fine.
    rng::JavaRandom random(seed);
    const auto noise = noise::BlendedNoise::legacy(random, parameters);

    if (mode == "fine") {
        // Sub-block steps along one column: see the .mjs beside this.
        for (int i = 0; i <= 512; ++i) {
            const double y = i / 64.0;
            std::printf("%.17g %.17g\n", y, noise.sample(11, y, 23));
        }
    } else if (mode == "columns") {
        for (int c = 0; c < 64; ++c) {
            const double x = (c % 8) * 71, z = (c / 8) * 89;
            for (int y = -256; y < 256; ++y) { std::printf("%.17g\n", noise.sample(x, y, z)); }
        }
    } else if (mode == "lines") {
        for (int k = 0; k < 32; ++k) {
            const double z = k * 977;
            for (int x = 0; x < 4096; ++x) { std::printf("%.17g\n", noise.sample(x, 0, z)); }
        }
    } else if (mode == "planes") {
        for (const int y : {-64, -32, 0, 1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 320}) {
            for (int i = 0; i < 4096; ++i) {
                std::printf("%d %.17g\n", y, noise.sample((i % 64) * 71, y, (i / 64) * 89));
            }
        }
    } else {
        std::fprintf(stderr, "mode must be 'lines', 'planes', 'columns' or 'fine'\n");
        return 2;
    }
    return 0;
}
