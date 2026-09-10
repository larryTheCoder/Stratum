// Stratum — does minecraft:clay_bands_offset explain bandlands' per-column
// shift?
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Throwaway: samples the vanilla pack's own clay_bands_offset noise (already
// buildable via this project's NoiseRegistry, the same machinery every other
// overworld noise goes through) at the exact grid bandlands-dump measured,
// and prints both series so they can be correlated against offsets.csv.
//
//   bandlands-offset-check <pack-dir> <seed> <span>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: bandlands-offset-check <pack-dir> <seed> <span>\n");
        return 2;
    }
    const auto pack = stratum::data::Pack::open(argv[1]);
    const std::int64_t seed = std::atoll(argv[2]);
    const int span = std::atoi(argv[3]);

    const std::vector<stratum::data::ResourceLocation> wanted{
        stratum::data::ResourceLocation::parse("minecraft:clay_bands_offset")};
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, wanted, seed, stratum::density::RandomSource::Xoroshiro);
    const auto& noise = noises.get(wanted[0]);

    std::printf("x,z,raw_y0,raw_y_neg64,raw_y64\n");
    for (int z = 0; z < span; ++z) {
        for (int x = 0; x < span; ++x) {
            const double y0 = noise.sample(x, 0, z);
            const double yn64 = noise.sample(x, -64, z);
            const double y64 = noise.sample(x, 64, z);
            std::printf("%d,%d,%.17g,%.17g,%.17g\n", x, z, y0, yn64, y64);
        }
    }
    return 0;
}
