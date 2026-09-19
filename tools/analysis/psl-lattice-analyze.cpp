// Stratum — where a spatially varying preliminary_surface_level is SAMPLED.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// THE OPEN ITEM. `terrain::ChunkFiller` evaluates the router's
// `preliminary_surface_level` entry per column. The server does not: driven by
// a three-valued `range_choice` (-40 / 0 / 60) the value that reaches
// `above_preliminary_surface` comes back as 101 distinct integers spanning the
// whole range, so it is sampled somewhere coarser than a column and blended.
// This reads that back off the server's own regions and measures the sampling.
//
// THE READOUT is the same one `aps-boundary-analyze.cpp` uses, and it is exact
// rather than a fit: every probe column is solid from the world floor up and
// its only surface rule paints a marker wherever the condition holds, so the
// LOWEST marker in a column is the condition's boundary at single-block
// resolution. The boundary is measured:
//
//     boundary(x, z) = psl(x, z) + surfaceDepth(x, z) - 8        (SPEC §11)
//
// so `psl(x, z) = boundary - surfaceDepth + 8` recovers the integer the server
// used, per column, with no model of the sampling in it at all. Everything
// below scores models against THAT.
//
// THE FIELD each probe dimension drives the entry with is rebuilt here from
// `stratum:probe_noise` — one octave, first octave -3 — exactly as
// `aquifer-nearsurface-analyze.cpp` rebuilds it, so the analysis can evaluate
// the field at any (x, z) rather than only where the server wrote a block.
// The field parameters are NOT guessed from the directory name: they are read
// out of the `spec.json` the probe recorded next to its regions, so a probe
// and its analysis cannot drift apart.
//
// SEVEN MODES, and the first two are deliberately independent of each other —
// the point of the exercise is to separate the lattice's PITCH from its
// ANCHOR rather than to fit them together:
//
//   pitch  Translation equivariance, and it needs no model of the blending.
//          `psl-lattice-probe.sh`'s `s_*` family drives the entry with the
//          SAME field shifted by c blocks in x (a `shifted_noise` shift, so
//          the shift is exact). If the server read the entry per column then
//          psl_c(x, z) == psl_0(x + c, z) for every c. If it samples on a
//          lattice of pitch P then that holds when P divides c and fails
//          otherwise, because shifting the field does not move the lattice.
//          A staircase in c, read with a denominator, and no interpolation
//          rule is assumed anywhere in it.
//
//   anchor Given a pitch, where the lattice sits. Scores every phase in
//          [0, P) against the measured psl, so the anchor is one scan over
//          one parameter rather than a corner of a joint fit.
//
//   fit    The joint grid search, kept as a CHECK on the two above rather
//          than as the measurement: pitch x anchor x blending rule x where
//          the floor falls. Reports the whole table, not just its argmax.
//
//   blends Every blending rule at the measured pitch and anchor, so each
//          refutation carries its own denominator rather than only losing a
//          joint fit. This is the mode the `f_*` dimensions are read with.
//
//   same   Column-by-column agreement between two probe dimensions. This is
//          what turns "cell width does not matter" and "flat_cache does not
//          matter" into counts: two dimensions of one world, one differing
//          setting, 36864 columns each.
//
//   census The distinct values the recovered psl takes, and whether they are
//          contiguous. A blend of arms {-40, 0, 60} on a COARSE pitch leaves
//          gaps after the floor and on a fine one does not, so the 101
//          contiguous integers are themselves a bound on P before any fit.
//
//   golden The two readings side by side on real overworld regions: the
//          `grass_block`s the server placed below the old per-column boundary,
//          how many each reading's band reaches, and the residual. Run it once
//          per seed — the seed argument builds the world.
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated -I build/dev/_deps/nlohmann_json-src/single_include tools/analysis/psl-lattice-analyze.cpp -L build/dev/lib -lstratum_core -lz -o build/psl-lattice-analyze
//   build/psl-lattice-analyze census .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/probes/apsb v_psl
//   build/psl-lattice-analyze pitch  .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/probes/psllat
//   build/psl-lattice-analyze anchor .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/probes/psllat s_c00
//   build/psl-lattice-analyze fit    .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/probes/psllat s_c00
//   build/psl-lattice-analyze blends .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/probes/psllat f_half f_quart
//   build/psl-lattice-analyze same   .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/probes/apsb4 w_sh1 w_sh2
//   build/psl-lattice-analyze golden .fixtures/1.21.11/worldgen 42 .fixtures/1.21.11/regions/seed-42/overworld/r.0.0.mca
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace stratum;

namespace {

constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kMaxY = 320;

/// The vanilla overworld at one seed, for the one quantity a probe world
/// cannot hand over: the surface depth the boundary formula subtracts. It is
/// the engine's own `surface::Executor`, not a replica written here.
class Depths {
public:
    Depths(const std::filesystem::path& tree, std::int64_t seed)
        : pack_(data::Pack::open(tree)), loaded_(settings::loadAll(pack_)),
          overworld_(loaded_.settings.at(data::ResourceLocation::parse("minecraft:overworld"))),
          rules_(surface::RuleGraph::resolve(
              overworld_.surfaceRule, data::ResourceLocation::parse("minecraft:overworld"))),
          noises_(density::NoiseRegistry::create(pack_, wanted(), seed,
                                                 density::RandomSource::Xoroshiro)),
          executor_(surface::Executor::compile(rules_, seed, overworld_.geometry, &noises_,
                                               overworld_.seaLevel)) {}

    [[nodiscard]] std::int32_t at(std::int32_t x, std::int32_t z) const {
        return executor_.surfaceDepth(x, z);
    }

    /// The router entry, per column, floored — the reading this change
    /// replaces, kept so the re-scoring can report both sides.
    [[nodiscard]] std::int32_t pslPerColumn(std::int32_t x, std::int32_t z) const {
        return static_cast<std::int32_t>(std::floor(rawPsl(x, z)));
    }

    /// The same entry through the MEASURED lattice: sampled every 16 blocks
    /// from the world origin, blended linearly in x and z, floored after.
    [[nodiscard]] std::int32_t pslLattice(std::int32_t x, std::int32_t z) const {
        constexpr std::int32_t kPitch = 16;
        const std::int32_t x0 = javamath::floorDiv(x, kPitch) * kPitch;
        const std::int32_t z0 = javamath::floorDiv(z, kPitch) * kPitch;
        const double u = static_cast<double>(x - x0) / static_cast<double>(kPitch);
        const double v = static_cast<double>(z - z0) / static_cast<double>(kPitch);
        // Floored AT the sample: measured by the `f_*` family, invisible on
        // vanilla's own integer-valued `find_top_surface` and the difference
        // between a right model and a lucky one on a data pack.
        const double c00 = std::floor(rawPsl(x0, z0));
        const double c10 = std::floor(rawPsl(x0 + kPitch, z0));
        const double c01 = std::floor(rawPsl(x0, z0 + kPitch));
        const double c11 = std::floor(rawPsl(x0 + kPitch, z0 + kPitch));
        const double low = c00 + ((c10 - c00) * u);
        const double high = c01 + ((c11 - c01) * u);
        return static_cast<std::int32_t>(std::floor(low + ((high - low) * v)));
    }

    [[nodiscard]] std::int32_t minY() const { return overworld_.geometry.minY; }
    [[nodiscard]] std::int32_t topY() const {
        return overworld_.geometry.minY + overworld_.geometry.height;
    }

private:
    [[nodiscard]] double rawPsl(std::int32_t x, std::int32_t z) const {
        const auto key = (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
        const auto found = pslCache_.find(key);
        if (found != pslCache_.end()) {
            return found->second;
        }
        const double value = interpreter_.evaluate(
            overworld_.router.at(settings::RouterEntry::PreliminarySurfaceLevel),
            density::Point{.x = x, .y = 0, .z = z});
        pslCache_.emplace(key, value);
        return value;
    }

    [[nodiscard]] std::vector<data::ResourceLocation> wanted() const {
        auto names = loaded_.graph.referencedNoises();
        const auto surfaceNoises = rules_.referencedNoises();
        names.insert(names.end(), surfaceNoises.begin(), surfaceNoises.end());
        names.push_back(data::ResourceLocation::parse("minecraft:surface"));
        names.push_back(data::ResourceLocation::parse("minecraft:surface_secondary"));
        names.push_back(data::ResourceLocation::parse("minecraft:clay_bands_offset"));
        return names;
    }

    data::Pack pack_;
    settings::LoadedSettings loaded_;
    settings::NoiseSettings overworld_;
    surface::RuleGraph rules_;
    density::NoiseRegistry noises_;
    surface::Executor executor_;
    density::Interpreter interpreter_{loaded_.graph, noises_,
                                      density::CellGeometry{
                                          .width = overworld_.geometry.cellWidth(),
                                          .height = overworld_.geometry.cellHeight()}};
    mutable std::map<std::int64_t, double> pslCache_;
};

/// The driving field of one probe dimension, as its `spec.json` records it:
/// a `range_choice` ladder over `stratum:probe_noise`, optionally shifted.
/// Read rather than inferred from the entry name — a probe whose analysis
/// hard-codes the parameters it was generated with is a probe that silently
/// measures the wrong thing the first time either side is edited.
struct Field {
    double xzScale = 1.0;
    double shiftX = 0.0;
    double shiftZ = 0.0;
    // Arms, low to high, with the thresholds between them. Two arms means one
    // threshold; three means two.
    std::vector<double> thresholds;
    std::vector<double> arms;

    [[nodiscard]] double at(const noise::NormalNoise& probe, double x, double z) const {
        const double n = probe.sample((x * xzScale) + shiftX, 0.0, (z * xzScale) + shiftZ);
        for (std::size_t i = 0; i < thresholds.size(); ++i) {
            if (n < thresholds[i]) {
                return arms[i];
            }
        }
        return arms.back();
    }
};

/// The field evaluated once per integer (x, z) over the window every model
/// below can reach. A grid search over pitch x anchor x blend re-reads the
/// same few hundred thousand lattice corners tens of thousands of times, and
/// the noise call is the whole cost of it.
class FieldGrid {
public:
    static constexpr std::int32_t kMargin = 64; // room for the coarsest lattice's far corner

    FieldGrid(const Field& field, const noise::NormalNoise& probe, std::int32_t baseX,
              std::int32_t baseZ)
        : lowX_(baseX - kMargin), lowZ_(baseZ - kMargin), span_(512 + (2 * kMargin)),
          values_(static_cast<std::size_t>(span_) * span_) {
        for (std::int32_t z = 0; z < span_; ++z) {
            for (std::int32_t x = 0; x < span_; ++x) {
                values_[(static_cast<std::size_t>(z) * span_) + x] =
                    field.at(probe, lowX_ + x, lowZ_ + z);
            }
        }
    }

    [[nodiscard]] double at(std::int32_t x, std::int32_t z) const {
        return values_[(static_cast<std::size_t>(z - lowZ_) * span_) + (x - lowX_)];
    }

private:
    std::int32_t lowX_;
    std::int32_t lowZ_;
    std::int32_t span_;
    std::vector<double> values_;
};

/// Peels the nested `range_choice` a probe's psl entry is, whatever it is
/// wrapped in. Throws rather than guessing: an entry this cannot read is an
/// entry whose measurement would be meaningless.
Field readField(const nlohmann::json& node) {
    Field field;
    const nlohmann::json* cursor = &node;
    // A bare constant is a legal entry and a useful control: no lattice can
    // show through it, which is the whole reason the boundary law could be
    // measured without this question being settled.
    if (cursor->is_number()) {
        field.arms.push_back(cursor->get<double>());
        return field;
    }
    // `flat_cache` and friends wrap the ladder without changing its shape.
    while (cursor->is_object() && cursor->contains("type") &&
           cursor->at("type").get<std::string>() != "minecraft:range_choice") {
        const auto type = cursor->at("type").get<std::string>();
        if (type == "minecraft:constant") {
            field.arms.push_back(cursor->at("argument").get<double>());
            return field;
        }
        if (!cursor->contains("argument")) {
            throw std::runtime_error("psl entry is not a range_choice ladder: " + type);
        }
        cursor = &cursor->at("argument");
    }
    bool haveNoise = false;
    while (cursor->is_object() && cursor->contains("type") &&
           cursor->at("type").get<std::string>() == "minecraft:range_choice") {
        const auto& input = cursor->at("input");
        const auto inputType = input.at("type").get<std::string>();
        if (inputType == "minecraft:noise") {
            if (!haveNoise) {
                field.xzScale = input.at("xz_scale").get<double>();
                haveNoise = true;
            }
        } else if (inputType == "minecraft:shifted_noise") {
            if (!haveNoise) {
                field.xzScale = input.at("xz_scale").get<double>();
                field.shiftX = input.at("shift_x").at("argument").get<double>();
                field.shiftZ = input.at("shift_z").at("argument").get<double>();
                haveNoise = true;
            }
        } else {
            throw std::runtime_error("psl ladder input is neither noise nor shifted_noise: " +
                                     inputType);
        }
        field.thresholds.push_back(cursor->at("max_exclusive").get<double>());
        field.arms.push_back(cursor->at("when_in_range").get<double>());
        const auto& out = cursor->at("when_out_of_range");
        if (out.is_number()) {
            field.arms.push_back(out.get<double>());
            return field;
        }
        cursor = &out;
    }
    throw std::runtime_error("psl ladder did not end in a constant arm");
}

/// Every dimension of a recorded probe spec, by name.
std::map<std::string, Field> readSpec(const std::filesystem::path& root) {
    std::ifstream in(root / "spec.json");
    if (!in) {
        throw std::runtime_error("no spec.json under " + root.string());
    }
    nlohmann::json spec;
    in >> spec;
    std::map<std::string, Field> fields;
    for (const auto& entry : spec) {
        if (!entry.contains("router") || !entry.at("router").contains("preliminary_surface_level")) {
            continue;
        }
        const auto& psl = entry.at("router").at("preliminary_surface_level");
        if (psl.is_number()) {
            continue; // a constant control: nothing to sample.
        }
        fields.emplace(entry.at("name").get<std::string>(), readField(psl));
    }
    return fields;
}

/// One probe dimension read back: the recovered psl per column, and a flag
/// for the columns whose marker band was not a single run (none have been
/// seen, but a silent one would poison every count below).
struct Readback {
    // A whole region, indexed in ABSOLUTE world coordinates: `--origin` moves
    // a probe into r.-1.-1, and the negative side is the only place where
    // floorDiv and a truncating division disagree about which lattice cell a
    // column is in.
    static constexpr std::int32_t kSide = 512;
    std::int32_t baseX = 0;
    std::int32_t baseZ = 0;
    std::vector<std::int32_t> psl = std::vector<std::int32_t>(kSide * kSide, INT32_MIN);
    long long columns = 0;
    long long broken = 0;

    [[nodiscard]] bool has(std::int32_t x, std::int32_t z) const {
        const std::int32_t lx = x - baseX;
        const std::int32_t lz = z - baseZ;
        return lx >= 0 && lz >= 0 && lx < kSide && lz < kSide && psl[(lz * kSide) + lx] != INT32_MIN;
    }
    [[nodiscard]] std::int32_t at(std::int32_t x, std::int32_t z) const {
        return psl[((z - baseZ) * kSide) + (x - baseX)];
    }
};

/// The one region a probe dimension wrote, and where in the world it sits.
/// Read off the file name rather than assumed to be r.0.0 — `--origin` is
/// what makes the anchor measurable at negative coordinates at all.
std::filesystem::path findRegion(const std::filesystem::path& dir, std::int32_t& baseX,
                                 std::int32_t& baseZ) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        int rx = 0;
        int rz = 0;
        if (std::sscanf(name.c_str(), "r.%d.%d.mca", &rx, &rz) == 2) {
            baseX = rx * 512;
            baseZ = rz * 512;
            return entry.path();
        }
    }
    throw std::runtime_error("no r.<x>.<z>.mca under " + dir.string());
}

Readback readBack(const Depths& depths, const std::filesystem::path& dir) {
    Readback out;
    const std::filesystem::path region = findRegion(dir, out.baseX, out.baseZ);
    const auto file = region::RegionFile::open(region);
    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk =
                chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    std::int32_t lowest = 0;
                    std::int32_t highest = 0;
                    long long marked = 0;
                    for (std::int32_t y = kMinY; y < kMaxY; ++y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr || block->name != "minecraft:diamond_block") {
                            continue;
                        }
                        if (marked == 0) {
                            lowest = y;
                        }
                        highest = y;
                        ++marked;
                    }
                    if (marked == 0) {
                        continue;
                    }
                    ++out.columns;
                    if (highest - lowest + 1 != marked) {
                        ++out.broken;
                        continue;
                    }
                    const std::int32_t x = out.baseX + (chunkX * 16) + localX;
                    const std::int32_t z = out.baseZ + (chunkZ * 16) + localZ;
                    out.psl[(((chunkZ * 16) + localZ) * Readback::kSide) + (chunkX * 16) +
                            localX] = lowest - depths.at(x, z) + 8;
                }
            }
        }
    }
    return out;
}

/// How a model turns lattice samples into a column's value.
enum class Blend : std::uint8_t {
    /// No blending: the column takes its cell's LOWER corner, the sample at
    /// (X0, Z0). This is "quantise to the cell corner", NOT "the nearest of
    /// the four corners" — an earlier label here and in SPEC called it the
    /// latter, which it has never been. The genuine nearest-corner model is
    /// scored, with its own denominator, in
    /// tests/conformance/vanilla_psl_lattice_test.cpp.
    None,
    Bilinear,      ///< linear in x and z, floored afterwards
    BilinearPre,   ///< each lattice sample floored first, then blended, then floored
    BilinearRound, ///< linear, rounded rather than floored
    /// Bilinear, but the cell a column belongs to is found with a TRUNCATING
    /// division instead of floorDiv. Identical everywhere at x, z >= 0 and
    /// wrong on the negative side, which is why `--origin` exists.
    BilinearTrunc,
    /// Each lattice sample floored first, blended, then TRUNCATED toward zero
    /// rather than floored. Separable only where the blend is negative and
    /// fractional, which is exactly what the `f_*` arms produce.
    BilinearPreTrunc,
    /// Each lattice sample floored first, blended, then rounded.
    BilinearPreRound,
    /// The samples TRUNCATED rather than floored before the blend. -0.5 goes
    /// to 0 instead of -1, so the `f_*` arms separate this too.
    BilinearTruncPre
};

const char* blendName(Blend blend) {
    switch (blend) {
    case Blend::None:
        return "lower corner, no blend";
    case Blend::Bilinear:
        return "bilinear, floor after";
    case Blend::BilinearPre:
        return "bilinear, floor before";
    case Blend::BilinearRound:
        return "bilinear, round after";
    case Blend::BilinearTrunc:
        return "bilinear, truncating cell";
    case Blend::BilinearPreTrunc:
        return "floor before, trunc after";
    case Blend::BilinearPreRound:
        return "floor before, round after";
    case Blend::BilinearTruncPre:
        return "trunc before, floor after";
    }
    return "?";
}

std::int32_t predict(const FieldGrid& grid, std::int32_t pitch, std::int32_t anchorX,
                     std::int32_t anchorZ, Blend blend, std::int32_t x, std::int32_t z) {
    const auto corner = [pitch, blend](std::int32_t v, std::int32_t anchor) {
        const std::int32_t offset = v - anchor;
        const std::int32_t cell = blend == Blend::BilinearTrunc ? offset / pitch
                                                                : javamath::floorDiv(offset, pitch);
        return (cell * pitch) + anchor;
    };
    const std::int32_t x0 = corner(x, anchorX);
    const std::int32_t z0 = corner(z, anchorZ);
    if (blend == Blend::None) {
        return static_cast<std::int32_t>(std::floor(grid.at(x0, z0)));
    }
    const double u = static_cast<double>(x - x0) / static_cast<double>(pitch);
    const double v = static_cast<double>(z - z0) / static_cast<double>(pitch);
    const bool floorFirst = blend == Blend::BilinearPre || blend == Blend::BilinearPreTrunc ||
                            blend == Blend::BilinearPreRound;
    auto sample = [&](std::int32_t sx, std::int32_t sz) {
        const double raw = grid.at(sx, sz);
        if (floorFirst) {
            return std::floor(raw);
        }
        if (blend == Blend::BilinearTruncPre) {
            return std::trunc(raw);
        }
        return raw;
    };
    const double c00 = sample(x0, z0);
    const double c10 = sample(x0 + pitch, z0);
    const double c01 = sample(x0, z0 + pitch);
    const double c11 = sample(x0 + pitch, z0 + pitch);
    const double top = c00 + ((c10 - c00) * u);
    const double bottom = c01 + ((c11 - c01) * u);
    const double value = top + ((bottom - top) * v);
    if (blend == Blend::BilinearRound || blend == Blend::BilinearPreRound) {
        return static_cast<std::int32_t>(std::llround(value));
    }
    if (blend == Blend::BilinearPreTrunc) {
        return static_cast<std::int32_t>(value);
    }
    return static_cast<std::int32_t>(std::floor(value));
}

struct Score {
    long long hits = 0;
    long long total = 0;
};

Score scoreModel(const Readback& back, const FieldGrid& grid, std::int32_t pitch,
                 std::int32_t anchorX, std::int32_t anchorZ, Blend blend) {
    Score score;
    for (std::int32_t z = back.baseZ; z < back.baseZ + Readback::kSide; ++z) {
        for (std::int32_t x = back.baseX; x < back.baseX + Readback::kSide; ++x) {
            if (!back.has(x, z)) {
                continue;
            }
            ++score.total;
            score.hits += static_cast<long long>(
                back.at(x, z) == predict(grid, pitch, anchorX, anchorZ, blend, x, z));
        }
    }
    return score;
}

// --- modes -----------------------------------------------------------------

void modeCensus(const Depths& depths, const std::filesystem::path& root,
                const std::vector<std::string>& names) {
    for (const auto& name : names) {
        const auto back = readBack(depths, root / name);
        std::map<std::int32_t, long long> histogram;
        for (std::int32_t z = back.baseZ; z < back.baseZ + Readback::kSide; ++z) {
            for (std::int32_t x = back.baseX; x < back.baseX + Readback::kSide; ++x) {
                if (back.has(x, z)) {
                    ++histogram[back.at(x, z)];
                }
            }
        }
        std::int32_t gaps = 0;
        if (!histogram.empty()) {
            for (std::int32_t v = histogram.begin()->first; v <= histogram.rbegin()->first; ++v) {
                gaps += static_cast<std::int32_t>(histogram.find(v) == histogram.end());
            }
        }
        std::printf("%s/%s  columns=%lld broken=%lld  distinct psl=%zu  range=[%d, %d]  gaps=%d\n",
                    root.filename().c_str(), name.c_str(), back.columns, back.broken,
                    histogram.size(), histogram.empty() ? 0 : histogram.begin()->first,
                    histogram.empty() ? 0 : histogram.rbegin()->first, gaps);
    }
}

void modeSame(const Depths& depths, const std::filesystem::path& root,
              const std::vector<std::string>& names) {
    if (names.size() < 2) {
        std::fprintf(stderr, "same needs at least two dimension names\n");
        return;
    }
    const auto first = readBack(depths, root / names[0]);
    for (std::size_t i = 1; i < names.size(); ++i) {
        const auto other = readBack(depths, root / names[i]);
        long long both = 0;
        long long same = 0;
        std::map<std::int32_t, long long> delta;
        for (std::int32_t z = first.baseZ; z < first.baseZ + Readback::kSide; ++z) {
            for (std::int32_t x = first.baseX; x < first.baseX + Readback::kSide; ++x) {
                if (!first.has(x, z) || !other.has(x, z)) {
                    continue;
                }
                ++both;
                same += static_cast<long long>(first.at(x, z) == other.at(x, z));
                ++delta[other.at(x, z) - first.at(x, z)];
            }
        }
        std::printf("%s vs %s: identical on %lld of %lld columns\n", names[0].c_str(),
                    names[i].c_str(), same, both);
        if (same != both) {
            std::printf("    difference histogram:");
            for (const auto& [d, count] : delta) {
                std::printf(" %d:%lld", d, count);
            }
            std::printf("\n");
        }
    }
}

/// Translation equivariance: the pitch, with no blending rule assumed.
///
/// `s_cNN` drives the entry with the base field shifted by NN blocks in x
/// (`t_cNN`: in z). If the server reads the entry per column, the shifted
/// world's psl at x is the base world's psl at x + NN, everywhere. If it
/// samples on a lattice of pitch P, that holds exactly when P divides NN.
void modePitch(const Depths& depths, const std::filesystem::path& root) {
    const auto fields = readSpec(root);
    std::map<std::string, Readback> worlds;
    for (const auto& [name, field] : fields) {
        (void)field;
        if (!std::filesystem::is_directory(root / name)) {
            continue;
        }
        worlds.emplace(name, readBack(depths, root / name));
    }
    for (const char axis : {'s', 't'}) {
        const std::string base = std::string(1, axis) + "_c00";
        const auto baseIt = worlds.find(base);
        if (baseIt == worlds.end()) {
            continue;
        }
        std::printf("shift family '%c' (%s in %c)\n", axis,
                    axis == 's' ? "field shifted" : "field shifted", axis == 's' ? 'x' : 'z');
        for (const auto& [name, back] : worlds) {
            if (name.size() != 5 || name[0] != axis || name[1] != '_' || name[2] != 'c') {
                continue;
            }
            const std::int32_t shift = std::atoi(name.substr(3).c_str());
            long long hits = 0;
            long long total = 0;
            for (std::int32_t z = back.baseZ; z < back.baseZ + Readback::kSide; ++z) {
                for (std::int32_t x = back.baseX; x < back.baseX + Readback::kSide; ++x) {
                    const std::int32_t bx = axis == 's' ? x + shift : x;
                    const std::int32_t bz = axis == 't' ? z + shift : z;
                    if (!back.has(x, z) || !baseIt->second.has(bx, bz)) {
                        continue;
                    }
                    ++total;
                    hits += static_cast<long long>(back.at(x, z) == baseIt->second.at(bx, bz));
                }
            }
            std::printf("    shift %2d: psl_c(x) == psl_0(x+c) on %lld of %lld columns (%.4f)\n",
                        shift, hits, total,
                        total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total));
        }
    }
}

void modeAnchor(const Depths& depths, const noise::NormalNoise& probe,
                const std::filesystem::path& root, const std::vector<std::string>& names,
                std::int32_t pitch) {
    const auto fields = readSpec(root);
    for (const auto& name : names) {
        const auto back = readBack(depths, root / name);
        const FieldGrid grid(fields.at(name), probe, back.baseX, back.baseZ);
        for (const Blend blend : {Blend::BilinearPre, Blend::Bilinear}) {
            // Every phase in [0, pitch) x [0, pitch), ranked. The anchor is
            // one scan over one parameter at the pitch the shift family
            // already measured, not a corner of a joint fit — and printing
            // the runners-up is the point: an anchor that wins by one column
            // is not an anchor.
            std::vector<std::tuple<long long, std::int32_t, std::int32_t>> ranked;
            for (std::int32_t anchorZ = 0; anchorZ < pitch; ++anchorZ) {
                for (std::int32_t anchorX = 0; anchorX < pitch; ++anchorX) {
                    const auto score = scoreModel(back, grid, pitch, anchorX, anchorZ, blend);
                    ranked.emplace_back(score.hits, anchorX, anchorZ);
                }
            }
            std::sort(ranked.rbegin(), ranked.rend());
            const long long best = std::get<0>(ranked.front());
            const auto perfect = static_cast<long long>(
                std::count_if(ranked.begin(), ranked.end(),
                              [best](const auto& row) { return std::get<0>(row) == best; }));
            std::printf("%s/%s  pitch=%d  %-24s  %lld phases of %zu reach the best score\n",
                        root.filename().c_str(), name.c_str(), pitch, blendName(blend), perfect,
                        ranked.size());
            for (std::size_t i = 0; i < std::min<std::size_t>(4, ranked.size()); ++i) {
                std::printf("      anchor (%2d,%2d): %lld of %lld\n", std::get<1>(ranked[i]),
                            std::get<2>(ranked[i]), std::get<0>(ranked[i]),
                            scoreModel(back, grid, pitch, 0, 0, blend).total);
            }
        }
    }
}

/// Every blending rule at the measured pitch and anchor, so a refutation has
/// its own denominator rather than only losing a joint fit.
void modeBlends(const Depths& depths, const noise::NormalNoise& probe,
                const std::filesystem::path& root, const std::vector<std::string>& names) {
    const auto fields = readSpec(root);
    for (const auto& name : names) {
        const auto back = readBack(depths, root / name);
        const FieldGrid grid(fields.at(name), probe, back.baseX, back.baseZ);
        std::printf("%s/%s at pitch 16, anchor (0,0)\n", root.filename().string().c_str(), name.c_str());
        for (const Blend blend :
             {Blend::Bilinear, Blend::BilinearPre, Blend::BilinearRound, Blend::BilinearTrunc,
              Blend::BilinearPreTrunc, Blend::BilinearPreRound, Blend::BilinearTruncPre,
              Blend::None}) {
            const auto score = scoreModel(back, grid, 16, 0, 0, blend);
            std::printf("    %-26s %lld of %lld\n", blendName(blend), score.hits, score.total);
        }
    }
}

void modeFit(const Depths& depths, const noise::NormalNoise& probe,
             const std::filesystem::path& root, const std::vector<std::string>& names) {
    const auto fields = readSpec(root);
    for (const auto& name : names) {
        const auto back = readBack(depths, root / name);
        const FieldGrid grid(fields.at(name), probe, back.baseX, back.baseZ);
        std::printf("%s/%s  columns=%lld broken=%lld\n", root.filename().string().c_str(), name.c_str(),
                    back.columns, back.broken);
        struct Best {
            Score score;
            std::int32_t pitch = 0;
            std::int32_t anchorX = 0;
            std::int32_t anchorZ = 0;
            Blend blend = Blend::None;
        };
        Best best;
        for (const std::int32_t pitch : {1, 2, 4, 8, 16, 32}) {
            for (const Blend blend : {Blend::None, Blend::Bilinear, Blend::BilinearPre,
                                      Blend::BilinearRound, Blend::BilinearTrunc,
                                      Blend::BilinearPreTrunc, Blend::BilinearPreRound,
                                      Blend::BilinearTruncPre}) {
                Best local;
                for (std::int32_t anchorZ = 0; anchorZ < pitch; ++anchorZ) {
                    for (std::int32_t anchorX = 0; anchorX < pitch; ++anchorX) {
                        const auto score = scoreModel(back, grid, pitch, anchorX, anchorZ, blend);
                        if (score.hits > local.score.hits) {
                            local = Best{score, pitch, anchorX, anchorZ, blend};
                        }
                    }
                }
                std::printf("    pitch %2d  %-22s best anchor (%d,%d): %lld of %lld (%.4f)\n",
                            pitch, blendName(blend), local.anchorX, local.anchorZ,
                            local.score.hits, local.score.total,
                            local.score.total == 0 ? 0.0
                                                   : static_cast<double>(local.score.hits) /
                                                         static_cast<double>(local.score.total));
                if (local.score.hits > best.score.hits) {
                    best = local;
                }
            }
        }
        std::printf("    BEST pitch %d anchor (%d,%d) %s: %lld of %lld\n", best.pitch,
                    best.anchorX, best.anchorZ, blendName(best.blend), best.score.hits,
                    best.score.total);
    }
}

/// The golden re-scoring, both readings side by side.
///
/// `above_preliminary_surface` gates the overworld's surface-materials
/// subtree and appears nowhere else in any of vanilla's seven dimensions, so
/// every `grass_block` the SERVER placed below the old per-column boundary is
/// a block that reading cannot produce at all. The measured band explains most
/// of them; the residual is what this lattice was opened to account for, and
/// the only honest test of the lattice on real terrain is whether that
/// residual SHRINKS.
void modeGolden(const Depths& depths, const std::vector<std::string>& regions) {
    long long grass = 0;
    long long belowOld = 0;
    long long insideColumn = 0;
    long long insideLattice = 0;
    std::map<std::int32_t, long long> latticeMinusColumn;

    for (const auto& path : regions) {
        const auto file = region::RegionFile::open(path);
        long long seedBelow = 0;
        long long seedColumn = 0;
        long long seedLattice = 0;
        for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto chunk =
                    chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        const std::int32_t column = depths.pslPerColumn(x, z);
                        const std::int32_t lattice = depths.pslLattice(x, z);
                        const std::int32_t depth = depths.at(x, z);
                        ++latticeMinusColumn[lattice - column];
                        for (std::int32_t y = depths.minY(); y < depths.topY(); ++y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr || found->name != "minecraft:grass_block") {
                                continue;
                            }
                            ++grass;
                            if (y >= column) {
                                continue;
                            }
                            ++belowOld;
                            ++seedBelow;
                            seedColumn += static_cast<long long>(y >= column + depth - 8);
                            seedLattice += static_cast<long long>(y >= lattice + depth - 8);
                        }
                    }
                }
            }
        }
        insideColumn += seedColumn;
        insideLattice += seedLattice;
        std::printf("%s  below old psl=%lld  inside band (per column)=%lld  (lattice)=%lld  "
                    "residual %lld -> %lld\n",
                    path.c_str(), seedBelow, seedColumn, seedLattice, seedBelow - seedColumn,
                    seedBelow - seedLattice);
    }
    std::printf("TOTAL grass=%lld  below old psl=%lld  inside band: per column %lld, lattice "
                "%lld  residual %lld -> %lld\n",
                grass, belowOld, insideColumn, insideLattice, belowOld - insideColumn,
                belowOld - insideLattice);
    std::printf("    lattice psl - per-column psl, over every column scored:");
    for (const auto& [delta, count] : latticeMinusColumn) {
        std::printf(" %d:%lld", delta, count);
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: psl-lattice-analyze <mode> <worldgen-tree> <seed> <probe-root> "
                     "[names...]\n"
                     "       modes: pitch | anchor | blends | fit | same | census | "
                     "golden\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::filesystem::path tree = argv[2];
    const std::int64_t seed = std::atoll(argv[3]);
    const std::filesystem::path root = argv[4];
    std::vector<std::string> names;
    for (int i = 5; i < argc; ++i) {
        names.emplace_back(argv[i]);
    }

    const Depths depths(tree, seed);

    // The probe's own noise, rebuilt from a throwaway pack holding only it —
    // one octave, first octave -3, the same way aquifer-nearsurface-analyze
    // rebuilds it.
    const std::filesystem::path packDir =
        std::filesystem::temp_directory_path() / "stratum-psl-lattice-pack";
    std::filesystem::create_directories(packDir / "data" / "stratum" / "worldgen" / "noise");
    {
        std::ofstream meta(packDir / "pack.mcmeta");
        meta << R"({"pack": {"pack_format": 94, "description": "psl lattice analyze"}})";
    }
    {
        std::ofstream noiseFile(packDir / "data" / "stratum" / "worldgen" / "noise" /
                                "probe_noise.json");
        noiseFile << R"({"firstOctave": -3, "amplitudes": [1.0]})";
    }
    const auto probePack = data::Pack::open(packDir);
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("stratum:probe_noise")};
    const auto probeNoises =
        density::NoiseRegistry::create(probePack, wanted, seed, density::RandomSource::Xoroshiro);
    const auto& probe = probeNoises.get(wanted[0]);

    if (names.empty() && mode != "pitch" && mode != "golden") {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory()) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
    }

    if (mode == "census") {
        modeCensus(depths, root, names);
    } else if (mode == "same") {
        modeSame(depths, root, names);
    } else if (mode == "pitch") {
        modePitch(depths, root);
    } else if (mode == "anchor") {
        modeAnchor(depths, probe, root, names, 16);
    } else if (mode == "blends") {
        modeBlends(depths, probe, root, names);
    } else if (mode == "fit") {
        modeFit(depths, probe, root, names);
    } else if (mode == "golden") {
        // `root` is the first region path here, `names` the rest.
        std::vector<std::string> regions{root.string()};
        regions.insert(regions.end(), names.begin(), names.end());
        modeGolden(depths, regions);
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }
    return 0;
}
