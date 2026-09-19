// Stratum — tests candidate RNG salts against
// tools/analysis/ore-vein-rng-dump.cpp's dataset (SPEC's M3 section, "ore
// veins... the RNG still open").
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SETTLED, and this file is kept as the instrument that settles it — and as
// the record of how it was wrong. The membership roll is
// `rng::positionalSourceFor(seed, "minecraft:ore").at(x, y, z).nextFloat()
// < 0.7`: 100.000% on 79790 candidates across both probe sets (SPEC's M3
// section).
//
// WHAT THIS FILE USED TO ASSUME, because it cost a long search. Every public
// description calls this a "30% membership roll", so this test hard-coded a
// threshold of 0.3 and swept salts against it — while the measured marginal
// touch rate, recorded in SPEC the whole time, was 69.729%. At the CORRECT
// salt a 0.3 threshold reads 60.294%: an unremarkable near-miss, well under
// the 85% per-seed bar below, and indistinguishable by eye from the ~58%
// uncorrelated baseline. So roughly 1226 candidates were swept and refuted
// against a threshold none of them could have passed, `"minecraft:ore"`
// among them. An instrument that can only be wrong in one direction refutes
// the right answer as confidently as the wrong ones.
//
// The threshold is a SWEEP now, not a constant, and both senses are reported
// at each one — so the 60.3% near-miss and the 100.0% answer print side by
// side for the same salt. This file still only scores the FIRST of the three
// draws, which is what it is for; scoring all three jointly against the block
// the server actually wrote is
// `tests/conformance/vanilla_ore_vein_test.cpp`'s job, and that is the test
// that can tell a right answer from a near-miss.
//
// Reports the aggregate agreement AND two ways of slicing it that catch
// artifacts an aggregate-only number hides:
//   - per-seed min/max: an aggregate hit that isn't ~uniform across every
//     seed is almost always an artifact of unequal per-seed sample counts,
//     not a real derivation. Caught exactly this on "minecraft:vein_gap":
//     71.6% aggregate, but 45%/74%/80%/60% per-seed — refuted once seen
//     per-seed. Per-seed n is printed alongside, because the probe seeds
//     contribute wildly unequal row counts (one contributes none at all).
//   - copper vs iron: tests whether the two vein types might use different
//     salts (a single combined test would dilute either type's real 100%
//     match down to a confusing ~65-80%, not obviously distinguishable
//     from noise, if the other type's salt were wrong).
// A genuine salt should read close to 100% in EVERY seed and EVERY type, not
// just on average.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       tools/analysis/ore-vein-rng-test.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/ore-vein-rng-test
//   build/ore-vein-rng-test dataset.csv salt1 salt2 ...
//   build/ore-vein-rng-test dataset.csv --file candidates.txt   (one salt/line;
//       silent unless a candidate's per-seed min clears 85% in some
//       direction, to make large word-list sweeps readable)
#include <stratum/rng/xoroshiro128.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace stratum;

namespace {
/// Swept for every salt. 0.7 is the confirmed membership threshold; 0.3 is
/// the one this file used to hard-code, kept so the near-miss it produces at
/// the correct salt stays visible next to the real answer.
constexpr float kThresholds[] = {0.3F, 0.7F};

struct Row {
    std::int64_t seed;
    std::int32_t x, y, z;
    bool touched;
};

struct Agreement {
    long long agree = 0;
    long long total = 0;
    std::map<std::int64_t, std::pair<long long, long long>> perSeed; // agree, total

    void record(const std::int64_t seed, const bool ok) {
        agree += ok ? 1 : 0;
        ++total;
        auto& p = perSeed[seed];
        p.first += ok ? 1 : 0;
        ++p.second;
    }

    [[nodiscard]] double rate() const { return 100.0 * double(agree) / double(total ? total : 1); }

    [[nodiscard]] std::pair<double, double> perSeedMinMax() const {
        double lo = 100.0;
        double hi = 0.0;
        for (const auto& [seed, p] : perSeed) {
            const double r = 100.0 * double(p.first) / double(p.second);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        return {lo, hi};
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ore-vein-rng-test <dataset.csv> <salt> [<salt> ...]\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<Row> rows;
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string field;
        Row row{};
        std::getline(ss, field, ',');
        row.seed = std::atoll(field.c_str());
        std::getline(ss, field, ',');
        row.x = std::atoi(field.c_str());
        std::getline(ss, field, ',');
        row.y = std::atoi(field.c_str());
        std::getline(ss, field, ',');
        row.z = std::atoi(field.c_str());
        std::getline(ss, field, ','); // ridged, unused here
        std::getline(ss, field, ',');
        row.touched = field == "1";
        rows.push_back(row);
    }
    std::fprintf(stderr, "loaded %zu rows\n", rows.size());

    std::vector<std::string> salts;
    bool quietMode = false;
    if (argc == 4 && std::string(argv[2]) == "--file") {
        quietMode = true;
        std::ifstream saltFile(argv[3]);
        std::string saltLine;
        while (std::getline(saltFile, saltLine)) {
            if (!saltLine.empty()) {
                salts.push_back(saltLine);
            }
        }
        std::fprintf(stderr, "loaded %zu candidate salts from %s\n", salts.size(), argv[3]);
    } else {
        for (int argi = 2; argi < argc; ++argi) {
            salts.emplace_back(argv[argi]);
        }
    }

    long long checked = 0;
    long long hits = 0;
    for (const std::string& salt : salts) {
        // Both senses at every threshold in the sweep. 0.7 is the confirmed
        // one; 0.3 is kept because it is what this file used to test, and
        // seeing 60.3% next to 100.0% is the whole lesson in one line.
        for (const float threshold : kThresholds) {
            ++checked;
            Agreement less, geq, lessCu, geqCu, lessFe, geqFe;
            std::int64_t cachedSeed = 0;
            bool haveCached = false;
            rng::PositionalSource cachedSource{rng::Seed128{.lo = 1, .hi = 1}};
            for (const Row& row : rows) {
                if (!haveCached || row.seed != cachedSeed) {
                    cachedSource = rng::positionalSourceFor(row.seed, salt);
                    cachedSeed = row.seed;
                    haveCached = true;
                }
                auto gen = cachedSource.at(row.x, row.y, row.z);
                const float draw = gen.nextFloat();
                const bool okLess = (draw < threshold) == row.touched;
                const bool okGeq = (draw >= threshold) == row.touched;
                less.record(row.seed, okLess);
                geq.record(row.seed, okGeq);
                if (row.y >= 0) { // copper range; iron never reaches y>=0 (upper bound -8)
                    lessCu.record(row.seed, okLess);
                    geqCu.record(row.seed, okGeq);
                } else {
                    lessFe.record(row.seed, okLess);
                    geqFe.record(row.seed, okGeq);
                }
            }
            const auto [lessLo, lessHi] = less.perSeedMinMax();
            const auto [geqLo, geqHi] = geq.perSeedMinMax();
            // A real hit clears 85% even on its WORST seed — not just on
            // average, which is exactly what let "minecraft:vein_gap" through
            // before per-seed reporting existed.
            const bool isHit = lessLo >= 85.0 || geqLo >= 85.0;
            if (isHit) {
                ++hits;
            }
            if (isHit || !quietMode) {
                std::printf("salt=%-24s t=%.2f  all: <t=%.3f%%[%.0f-%.0f] >=t=%.3f%%[%.0f-%.0f]"
                            "   Cu: <t=%.3f%%   Fe: <t=%.3f%%%s\n",
                            salt.c_str(), double(threshold), less.rate(), lessLo, lessHi, geq.rate(),
                            geqLo, geqHi, lessCu.rate(), lessFe.rate(), isHit ? "   <-- HIT" : "");
            }
        }
    }
    if (quietMode) {
        std::fprintf(stderr, "checked %lld candidates, %lld hit (per-seed min >= 85%%)\n", checked, hits);
    }
    return 0;
}
