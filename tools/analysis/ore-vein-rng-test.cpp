// Stratum — tests candidate RNG salts against
// tools/analysis/ore-vein-rng-dump.cpp's dataset (SPEC's M3 section, "ore
// veins... the RNG still open").
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The hypothesis: the membership roll is
// `rng::positionalSourceFor(seed, SALT).at(x, y, z).nextFloat()`, compared
// against 0.3 one way or the other — the exact shape
// `surface::rule_graph.hpp`'s own `verticalGradientFires` already uses and
// has confirmed against 27 million real blocks. What is NOT known is SALT,
// or which side of 0.3 means "touched".
//
// Reports the aggregate agreement AND two ways of slicing it that catch
// artifacts an aggregate-only number hides:
//   - per-seed min/max: an aggregate hit that isn't ~uniform across every
//     seed is almost always an artifact of unequal per-seed sample counts,
//     not a real derivation. Caught exactly this on "minecraft:vein_gap":
//     71.6% aggregate, but 45%/74%/80%/60% per-seed — refuted once seen
//     per-seed.
//   - copper vs iron: tests whether the two vein types might use different
//     salts (a single combined test would dilute either type's real 100%
//     match down to a confusing ~65-80%, not obviously distinguishable
//     from noise, if the other type's salt were wrong).
// A genuine salt should read close to 100% (or close to 0%, i.e. 100% on
// the other direction) in EVERY seed and EVERY type, not just on average.
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
            const bool okLess = (draw < 0.3F) == row.touched;
            const bool okGeq = (draw >= 0.3F) == row.touched;
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
            std::printf("salt=%-30s all: <0.3=%.1f%%[%.0f-%.0f] >=0.3=%.1f%%[%.0f-%.0f]"
                        "   Cu: <0.3=%.1f%% >=0.3=%.1f%%   Fe: <0.3=%.1f%% >=0.3=%.1f%%%s\n",
                        salt.c_str(), less.rate(), lessLo, lessHi, geq.rate(), geqLo, geqHi,
                        lessCu.rate(), geqCu.rate(), lessFe.rate(), geqFe.rate(),
                        isHit ? "   <-- HIT" : "");
        }
    }
    if (quietMode) {
        std::fprintf(stderr, "checked %lld candidates, %lld hit (per-seed min >= 85%%)\n", checked, hits);
    }
    return 0;
}
