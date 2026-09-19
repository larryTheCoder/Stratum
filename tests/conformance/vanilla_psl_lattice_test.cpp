// Stratum — where a varying preliminary_surface_level is SAMPLED, measured.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `vanilla_above_preliminary_surface_test.cpp` measured what the condition
// compares y against and left one thing open: with a spatially varying
// `preliminary_surface_level` the value reaching it is not the router entry at
// the column. This file closes that, and it is written to be read by someone
// checking whether it was closed or merely declared closed — every refusal
// below carries its own denominator.
//
// WHAT THE PROBES ARE. `tools/analysis/psl-lattice-probe.sh`, three specs.
// Each forceloads an 8x8-chunk square — 128 blocks a side — and the server
// generates a border of chunks around it; the readback then reads whatever
// the ONE copied region file carries, which is neither the square nor the
// region. Measured from the marked columns themselves, in
// "every dimension of the sweep, scored twice and with its extent measured":
//
//   probes/psllat   seed 42,    24 dimensions, x, z in [0, 191]    (36864)
//   probes/psllat2  seed 31337,  9 of the same, x, z in [0, 191]   (36864)
//   probes/psllat3  seed 42,    10 of the same at chunk -12,
//                               x, z in [-256, -1], all of r.-1.-1 (65536)
//
// (`psllat` and `psllat2` are forceloaded at chunk 0, so the negative half of
// their border lies in r.-1.-1, which is not copied; `psllat3`'s border lies
// entirely inside its own region, which is why its readback is larger.)
//
// Each is `aps-boundary-probe.sh`'s readout — solid column, one surface rule
// painting a marker wherever `above_preliminary_surface` holds — so the band's
// lowest block is the condition's boundary and
// `psl = boundary - surfaceDepth + 8` recovers the integer the server used,
// per column, from the measured boundary law.
//
// WHAT IS MEASURED, one case each:
//
//   THE PITCH, and it is measured by TRANSLATION rather than by a fit. The
//   `s_cNN` dimensions drive the entry with the same field shifted NN blocks
//   in x (`t_cNN`: in z), so a per-column read would give
//   psl_NN(x, z) == psl_0(x+NN, z) for every NN. It holds on every column for
//   NN in {0, 16, 32} and on 2 to 21 percent of them for NN in {1..8, 12} —
//   which is 16, in both axes, at two seeds, with no interpolation rule
//   assumed anywhere in the argument.
//
//   THE ANCHOR, as one scan over one parameter at that pitch: exactly 1 of
//   the 256 phases reproduces every column, and it is the world origin. The
//   runner-up gets 7542 of 36864.
//
//   THE BLEND, and WHERE THE FLOOR FALLS. Linear in x and z; each lattice
//   sample floored where it is taken; the blend floored again. The `f_*`
//   dimensions are the only probe of this entry whose arms are not integers,
//   and so the only one that could ever separate those: flooring only after
//   the blend scores 20423 of 36864, truncating at the sample 3119,
//   truncating after the blend 4764, rounding after it 22010, quantising to
//   the cell's lower corner 21039 and to the nearest of the four 21103.
//   `f_half` and `f_quart` are NOT two observations of this — they are the
//   same server data, which is measured — so the placement is re-measured at
//   seed 31337 and again below zero.
//
//   THE CELL INDEX, at NEGATIVE coordinates. floorDiv and a truncating
//   division are the same function while x, z >= 0, so `probes/psllat3` runs
//   at chunk -12: floorDiv keeps 65536 of 65536 and truncation drops to 4216.
//
//   THAT IT IS NOT THE CELL LATTICE. `probes/apsb4` varies cell width 4/8/16,
//   cell height, and wraps the entry in `flat_cache`, all with the varying
//   field: 36864 of 36864 columns identical each time.
//
//   EVERY DIMENSION, TWICE. All 43 dimensions of the three specs are
//   reproduced exactly against the probe-noise replica — 1871872 columns —
//   and every interior column is ALSO predicted from the server's own
//   recovered psl at the four multiples of 16 around it, which needs no
//   replica, no anchor and no calibration of the first floor: 1598208 of
//   1598208, and 185856 more over `apsb4`.
//
//   AND THE ONE THAT MATTERS ON REAL TERRAIN, in BOTH directions. Forward:
//   of all 627766 `grass_block`s the server wrote in the eight golden
//   regions, not one falls below the band the lattice opens (the per-column
//   band leaves 2429 of the 14008 below its own boundary unexplained).
//   Reverse: 12916 column surfaces land inside the band the lattice opens
//   and 12403 of them carry a block only the gated subtree places (96.03%),
//   against 10676 of 11953 (89.32%) for the per-column band. Neither
//   direction is decisive alone, which is why both are run.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

using stratum::data::ResourceLocation;
using stratum::terrain::ChunkFiller;

constexpr std::int32_t kPitch = ChunkFiller::kPreliminarySurfacePitch;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

[[nodiscard]] bool haveWorldgen() {
    return std::filesystem::is_directory(fixtures() / "worldgen");
}

/// The vanilla overworld at one seed. Two things come out of it: the surface
/// depth the boundary law subtracts — the engine's own `surface::Executor`,
/// not a replica — and, for the golden case, the router's own
/// `preliminary_surface_level`.
class World {
public:
    explicit World(std::int64_t seed)
        : pack_(stratum::data::Pack::open(fixtures() / "worldgen")),
          loaded_(stratum::settings::loadAll(pack_)),
          overworld_(loaded_.settings.at(ResourceLocation::parse("minecraft:overworld"))),
          rules_(stratum::surface::RuleGraph::resolve(
              overworld_.surfaceRule, ResourceLocation::parse("minecraft:overworld"))),
          noises_(stratum::density::NoiseRegistry::create(
              pack_, wanted(), seed, stratum::density::RandomSource::Xoroshiro)),
          executor_(stratum::surface::Executor::compile(rules_, seed, overworld_.geometry, &noises_,
                                                        overworld_.seaLevel)),
          interpreter_(loaded_.graph, noises_,
                       stratum::density::CellGeometry{.width = overworld_.geometry.cellWidth(),
                                                      .height = overworld_.geometry.cellHeight()}) {
    }

    [[nodiscard]] std::int32_t surfaceDepth(std::int32_t x, std::int32_t z) const {
        return executor_.surfaceDepth(x, z);
    }

    /// The router entry per column, floored — the reading this replaced.
    [[nodiscard]] std::int32_t pslPerColumn(std::int32_t x, std::int32_t z) const {
        return static_cast<std::int32_t>(std::floor(rawPsl(x, z)));
    }

    /// The same entry through the shipped lattice: the filler's own helper,
    /// fed the four samples of the cell this column is in. Deliberately
    /// `ChunkFiller`'s function rather than a copy of it, so a change to the
    /// engine that this file does not follow shows up here as a failure.
    [[nodiscard]] std::int32_t pslLattice(std::int32_t x, std::int32_t z) const {
        const std::int32_t x0 = stratum::javamath::floorDiv(x, kPitch) * kPitch;
        const std::int32_t z0 = stratum::javamath::floorDiv(z, kPitch) * kPitch;
        const std::array<double, 4> corners{rawPsl(x0, z0), rawPsl(x0 + kPitch, z0),
                                            rawPsl(x0, z0 + kPitch),
                                            rawPsl(x0 + kPitch, z0 + kPitch)};
        return ChunkFiller::preliminarySurfaceIn(corners, x - x0, z - z0);
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
            overworld_.router.at(stratum::settings::RouterEntry::PreliminarySurfaceLevel),
            stratum::density::Point{.x = x, .y = 0, .z = z});
        pslCache_.emplace(key, value);
        return value;
    }

    [[nodiscard]] std::vector<ResourceLocation> wanted() const {
        auto names = loaded_.graph.referencedNoises();
        const auto surface = rules_.referencedNoises();
        names.insert(names.end(), surface.begin(), surface.end());
        names.push_back(ResourceLocation::parse("minecraft:surface"));
        names.push_back(ResourceLocation::parse("minecraft:surface_secondary"));
        names.push_back(ResourceLocation::parse("minecraft:clay_bands_offset"));
        return names;
    }

    stratum::data::Pack pack_;
    stratum::settings::LoadedSettings loaded_;
    stratum::settings::NoiseSettings overworld_;
    stratum::surface::RuleGraph rules_;
    stratum::density::NoiseRegistry noises_;
    stratum::surface::Executor executor_;
    stratum::density::Interpreter interpreter_;
    mutable std::map<std::int64_t, double> pslCache_;
};

/// One probe dimension read back: the recovered psl at every column the
/// server actually painted, in ABSOLUTE world coordinates, since `--origin`
/// puts `psllat3` in r.-1.-1.
class Readback {
public:
    static constexpr std::int32_t kSide = 512;

    Readback(const World& world, const std::filesystem::path& dir) {
        const std::filesystem::path region = findRegion(dir);
        const auto file = stratum::region::RegionFile::open(region);
        for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto chunk = stratum::chunk::Chunk::decode(
                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        std::int32_t lowest = 0;
                        std::int32_t highest = 0;
                        long long marked = 0;
                        for (std::int32_t y = -64; y < 320; ++y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr || found->name != "minecraft:diamond_block") {
                                continue;
                            }
                            if (marked == 0) {
                                lowest = y;
                            }
                            highest = y;
                            ++marked;
                        }
                        // The region holds far more chunks than the probe
                        // forceloaded; the untouched ones carry no marker.
                        if (marked == 0) {
                            continue;
                        }
                        ++columns_;
                        if (highest - lowest + 1 != marked) {
                            ++broken_;
                            continue;
                        }
                        const std::int32_t x = baseX_ + (chunkX * 16) + localX;
                        const std::int32_t z = baseZ_ + (chunkZ * 16) + localZ;
                        psl_[index(x, z)] = lowest - world.surfaceDepth(x, z) + 8;
                    }
                }
            }
        }
    }

    [[nodiscard]] bool has(std::int32_t x, std::int32_t z) const {
        const std::int32_t lx = x - baseX_;
        const std::int32_t lz = z - baseZ_;
        return lx >= 0 && lz >= 0 && lx < kSide && lz < kSide && psl_[index(x, z)] != kNothing;
    }

    [[nodiscard]] std::int32_t at(std::int32_t x, std::int32_t z) const {
        return psl_[index(x, z)];
    }

    [[nodiscard]] long long columns() const { return columns_; }

    [[nodiscard]] long long broken() const { return broken_; }

    [[nodiscard]] std::int32_t baseX() const { return baseX_; }

    [[nodiscard]] std::int32_t baseZ() const { return baseZ_; }

private:
    static constexpr std::int32_t kNothing = -100000;

    [[nodiscard]] std::size_t index(std::int32_t x, std::int32_t z) const {
        return (static_cast<std::size_t>(z - baseZ_) * kSide) +
               static_cast<std::size_t>(x - baseX_);
    }

    [[nodiscard]] std::filesystem::path findRegion(const std::filesystem::path& dir) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            int rx = 0;
            int rz = 0;
            if (std::sscanf(entry.path().filename().string().c_str(), "r.%d.%d.mca", &rx, &rz) ==
                2) {
                baseX_ = rx * kSide;
                baseZ_ = rz * kSide;
                return entry.path();
            }
        }
        return dir / "r.0.0.mca";
    }

    std::int32_t baseX_ = 0;
    std::int32_t baseZ_ = 0;
    std::vector<std::int32_t> psl_ =
        std::vector<std::int32_t>(static_cast<std::size_t>(kSide) * kSide, kNothing);
    long long columns_ = 0;
    long long broken_ = 0;
};

/// One probe dimension's driving field, as its own `spec.json` records it —
/// read rather than restated here, so the probe and the scoring cannot drift.
struct Field {
    double xzScale = 1.0;
    double shiftX = 0.0;
    double shiftZ = 0.0;
    std::vector<double> thresholds;
    std::vector<double> arms;

    [[nodiscard]] double at(const stratum::noise::NormalNoise& probe, std::int32_t x,
                            std::int32_t z) const {
        const double n = probe.sample((static_cast<double>(x) * xzScale) + shiftX, 0.0,
                                      (static_cast<double>(z) * xzScale) + shiftZ);
        for (std::size_t i = 0; i < thresholds.size(); ++i) {
            if (n < thresholds[i]) {
                return arms[i];
            }
        }
        return arms.back();
    }
};

[[nodiscard]] Field readField(const nlohmann::json& node) {
    Field field;
    const nlohmann::json* cursor = &node;
    while (cursor->is_object() && cursor->contains("type") &&
           cursor->at("type").get<std::string>() != "minecraft:range_choice") {
        if (cursor->at("type").get<std::string>() == "minecraft:constant") {
            field.arms.push_back(cursor->at("argument").get<double>());
            return field;
        }
        cursor = &cursor->at("argument");
    }
    bool haveNoise = false;
    while (cursor->is_object() && cursor->contains("type") &&
           cursor->at("type").get<std::string>() == "minecraft:range_choice") {
        const auto& input = cursor->at("input");
        if (!haveNoise) {
            field.xzScale = input.at("xz_scale").get<double>();
            if (input.at("type").get<std::string>() == "minecraft:shifted_noise") {
                field.shiftX = input.at("shift_x").at("argument").get<double>();
                field.shiftZ = input.at("shift_z").at("argument").get<double>();
            }
            haveNoise = true;
        }
        field.thresholds.push_back(cursor->at("max_exclusive").get<double>());
        field.arms.push_back(cursor->at("when_in_range").get<double>());
        const auto& out = cursor->at("when_out_of_range");
        if (out.is_number()) {
            field.arms.push_back(out.get<double>());
            break;
        }
        cursor = &out;
    }
    return field;
}

[[nodiscard]] std::map<std::string, Field> readSpec(const std::filesystem::path& root) {
    std::map<std::string, Field> fields;
    const auto spec = nlohmann::json::parse(std::ifstream(root / "spec.json"));
    for (const auto& entry : spec) {
        if (!entry.contains("router") ||
            !entry.at("router").contains("preliminary_surface_level")) {
            continue;
        }
        fields.emplace(entry.at("name").get<std::string>(),
                       readField(entry.at("router").at("preliminary_surface_level")));
    }
    return fields;
}

/// The probe's own noise, rebuilt from the manifest the probe wrote — one
/// octave, so nothing about the replica is in doubt.
[[nodiscard]] stratum::noise::NormalNoise probeNoise(const std::filesystem::path& root) {
    const auto manifest = nlohmann::json::parse(std::ifstream(root / "manifest.json"));
    const auto& declared = manifest.at("probe_noise");
    auto random = stratum::rng::XoroshiroPositionalFactory(manifest.at("seed").get<std::int64_t>())
                      .fromHashOf(declared.at("id").get<std::string>());
    return stratum::noise::NormalNoise::create(
        random, declared.at("first_octave").get<int>(),
        declared.at("amplitudes").get<std::vector<double>>());
}

/// How a candidate turns four lattice samples into a column's integer. The
/// shipped reading is `Model::Measured`, which is `ChunkFiller`'s own helper;
/// the rest exist to be refused with a denominator.
enum class Model : std::uint8_t {
    Measured,    ///< floor at the sample, blend, floor after — the shipped one
    FloorAfter,  ///< blend the raw samples, floor once at the end
    TruncBefore, ///< truncate at the sample instead of flooring
    TruncAfter,  ///< floor at the sample, truncate after the blend
    RoundAfter,  ///< floor at the sample, round after the blend
    /// No blending at all, quantising the column to the cell's LOWER corner
    /// — `corners[0]`, the sample at (X0, Z0). This is the
    /// "snap to the cell corner" reading, which is what a `flat_cache`-like
    /// relocation of the argument would produce; it is NOT "the nearest of
    /// the four corners", which is the row below.
    LowerCorner,
    /// No blending either, but the nearest of the FOUR samples: the corner
    /// chosen per axis by whether the offset has reached half a cell (ties
    /// at exactly 8 blocks go to the far corner). A distinct function from
    /// LowerCorner on three quarters of every cell, and it needs its own
    /// denominator rather than being lumped in with it.
    NearestCorner,
    TruncatingCell ///< the measured blend, but the cell found by truncating division
};

[[nodiscard]] std::int32_t predict(const Field& field, const stratum::noise::NormalNoise& probe,
                                   Model model, std::int32_t anchorX, std::int32_t anchorZ,
                                   std::int32_t x, std::int32_t z) {
    const auto corner = [model](std::int32_t v, std::int32_t anchor) {
        const std::int32_t offset = v - anchor;
        const std::int32_t cell = model == Model::TruncatingCell
                                      ? offset / kPitch
                                      : stratum::javamath::floorDiv(offset, kPitch);
        return (cell * kPitch) + anchor;
    };
    const std::int32_t x0 = corner(x, anchorX);
    const std::int32_t z0 = corner(z, anchorZ);
    const auto convert = [model](double raw) {
        switch (model) {
            case Model::FloorAfter:
                return raw;
            case Model::TruncBefore:
                return std::trunc(raw);
            default:
                return std::floor(raw);
        }
    };
    const std::array<double, 4> corners{convert(field.at(probe, x0, z0)),
                                        convert(field.at(probe, x0 + kPitch, z0)),
                                        convert(field.at(probe, x0, z0 + kPitch)),
                                        convert(field.at(probe, x0 + kPitch, z0 + kPitch))};
    if (model == Model::LowerCorner) {
        return static_cast<std::int32_t>(corners[0]);
    }
    if (model == Model::NearestCorner) {
        const std::size_t pick = static_cast<std::size_t>((x - x0) * 2 >= kPitch) +
                                 (static_cast<std::size_t>((z - z0) * 2 >= kPitch) * 2U);
        return static_cast<std::int32_t>(corners[pick]);
    }
    if (model == Model::Measured && anchorX == 0 && anchorZ == 0) {
        // The shipped helper itself, on the path the engine actually takes.
        return ChunkFiller::preliminarySurfaceIn(corners, x - x0, z - z0);
    }
    const double u = static_cast<double>(x - x0) / static_cast<double>(kPitch);
    const double v = static_cast<double>(z - z0) / static_cast<double>(kPitch);
    const double low = corners[0] + ((corners[1] - corners[0]) * u);
    const double high = corners[2] + ((corners[3] - corners[2]) * u);
    const double blended = low + ((high - low) * v);
    switch (model) {
        case Model::TruncAfter:
            return static_cast<std::int32_t>(blended);
        case Model::RoundAfter:
            return static_cast<std::int32_t>(std::llround(blended));
        default:
            return static_cast<std::int32_t>(std::floor(blended));
    }
}

struct Score {
    long long hits = 0;
    long long total = 0;
};

[[nodiscard]] Score score(const Readback& back, const Field& field,
                          const stratum::noise::NormalNoise& probe, Model model,
                          std::int32_t anchorX = 0, std::int32_t anchorZ = 0) {
    Score out;
    for (std::int32_t z = back.baseZ(); z < back.baseZ() + Readback::kSide; ++z) {
        for (std::int32_t x = back.baseX(); x < back.baseX() + Readback::kSide; ++x) {
            if (!back.has(x, z)) {
                continue;
            }
            ++out.total;
            out.hits += static_cast<long long>(
                back.at(x, z) == predict(field, probe, model, anchorX, anchorZ, x, z));
        }
    }
    return out;
}

/// The MODEL-FREE reading, and the only scoring in this file that involves no
/// replica of anything: every interior column predicted from the SERVER's own
/// recovered psl at the four multiples of 16 around it.
///
/// It needs no probe noise (the corner values come from the server), no
/// anchor scan (the corners are taken at multiples of 16 because that is what
/// the pitch case measured), and no calibration of where the first floor
/// falls (the recovered corner values are already integers — at a lattice
/// corner the blend is the corner). What is left being tested is exactly the
/// blend: the claim that an interior column is the bilinear blend of its
/// cell's four corners, floored.
///
/// A column is scored only when all four of its corners were recovered, so
/// the last 16 blocks of the readback in each axis fall out of the
/// denominator rather than being predicted from a corner that does not exist.
[[nodiscard]] Score cornerConsistency(const Readback& back) {
    Score out;
    for (std::int32_t z = back.baseZ(); z < back.baseZ() + Readback::kSide; ++z) {
        for (std::int32_t x = back.baseX(); x < back.baseX() + Readback::kSide; ++x) {
            if (!back.has(x, z)) {
                continue;
            }
            const std::int32_t x0 = stratum::javamath::floorDiv(x, kPitch) * kPitch;
            const std::int32_t z0 = stratum::javamath::floorDiv(z, kPitch) * kPitch;
            if (!back.has(x0, z0) || !back.has(x0 + kPitch, z0) || !back.has(x0, z0 + kPitch) ||
                !back.has(x0 + kPitch, z0 + kPitch)) {
                continue;
            }
            const double u = static_cast<double>(x - x0) / static_cast<double>(kPitch);
            const double v = static_cast<double>(z - z0) / static_cast<double>(kPitch);
            const double c00 = back.at(x0, z0);
            const double c10 = back.at(x0 + kPitch, z0);
            const double c01 = back.at(x0, z0 + kPitch);
            const double c11 = back.at(x0 + kPitch, z0 + kPitch);
            const double low = c00 + ((c10 - c00) * u);
            const double high = c01 + ((c11 - c01) * u);
            ++out.total;
            out.hits += static_cast<long long>(
                back.at(x, z) == static_cast<std::int32_t>(std::floor(low + ((high - low) * v))));
        }
    }
    return out;
}

/// The readback's own horizontal extent, measured from the marked columns
/// rather than restated from the probe script — the forceloaded square is
/// 128 blocks a side and the server generates chunks around it, so what a
/// dimension actually carries is neither the square nor the whole region.
struct Extent {
    std::int32_t minX = 0;
    std::int32_t maxX = 0;
    std::int32_t minZ = 0;
    std::int32_t maxZ = 0;
    long long columns = 0;
};

[[nodiscard]] Extent extentOf(const Readback& back) {
    Extent out;
    bool first = true;
    for (std::int32_t z = back.baseZ(); z < back.baseZ() + Readback::kSide; ++z) {
        for (std::int32_t x = back.baseX(); x < back.baseX() + Readback::kSide; ++x) {
            if (!back.has(x, z)) {
                continue;
            }
            ++out.columns;
            if (first) {
                out.minX = out.maxX = x;
                out.minZ = out.maxZ = z;
                first = false;
                continue;
            }
            out.minX = std::min(out.minX, x);
            out.maxX = std::max(out.maxX, x);
            out.minZ = std::min(out.minZ, z);
            out.maxZ = std::max(out.maxZ, z);
        }
    }
    return out;
}

/// psl_shift(x, z) against psl_0(x + shift, z) — the translation test, which
/// needs no model of the blend at all.
[[nodiscard]] Score equivariance(const Readback& shifted, const Readback& base, std::int32_t shift,
                                 bool alongX) {
    Score out;
    for (std::int32_t z = shifted.baseZ(); z < shifted.baseZ() + Readback::kSide; ++z) {
        for (std::int32_t x = shifted.baseX(); x < shifted.baseX() + Readback::kSide; ++x) {
            const std::int32_t bx = alongX ? x + shift : x;
            const std::int32_t bz = alongX ? z : z + shift;
            if (!shifted.has(x, z) || !base.has(bx, bz)) {
                continue;
            }
            ++out.total;
            out.hits += static_cast<long long>(shifted.at(x, z) == base.at(bx, bz));
        }
    }
    return out;
}

[[nodiscard]] bool haveProbe(const std::filesystem::path& root) {
    return std::filesystem::is_regular_file(root / "spec.json");
}

/// A block only the overworld's surface-materials subtree places — the same
/// list `vanilla_above_preliminary_surface_test.cpp` scores its reverse
/// direction with, so the two files' reverse counts are comparable. `stone`
/// is deliberately NOT on it: that subtree can place stone itself, so a
/// stone surface says nothing either way.
[[nodiscard]] bool gatedOnlyMaterial(const std::string& name) {
    static const std::vector<std::string> kMaterials{"minecraft:calcite",
                                                     "minecraft:coarse_dirt",
                                                     "minecraft:dirt",
                                                     "minecraft:grass_block",
                                                     "minecraft:gravel",
                                                     "minecraft:ice",
                                                     "minecraft:mud",
                                                     "minecraft:mycelium",
                                                     "minecraft:orange_terracotta",
                                                     "minecraft:packed_ice",
                                                     "minecraft:podzol",
                                                     "minecraft:powder_snow",
                                                     "minecraft:red_sand",
                                                     "minecraft:red_sandstone",
                                                     "minecraft:sand",
                                                     "minecraft:sandstone",
                                                     "minecraft:snow_block",
                                                     "minecraft:terracotta",
                                                     "minecraft:white_terracotta"};
    return std::find(kMaterials.begin(), kMaterials.end(), name) != kMaterials.end();
}

} // namespace

TEST_CASE("the preliminary surface level is sampled every 16 blocks, measured by translation",
          "[conformance][surface]") {
    // The PITCH, and the whole argument is translation equivariance: `s_cNN`
    // drives the router entry with the same field shifted NN blocks in x, so
    // a per-column read gives psl_NN(x, z) == psl_0(x + NN, z) for EVERY NN.
    // It does so only for the multiples of 16. No interpolation rule is
    // assumed anywhere in this case — that is the point of measuring the
    // pitch this way rather than reading it off a joint fit.
    const std::filesystem::path root = fixtures() / "probes" / "psllat";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat probe at " << root
                                   << "; generate it with tools/analysis/psl-lattice-probe.sh "
                                      "--accept-eula");
    }
    const World world{42};
    const Readback base(world, root / "s_c00");
    REQUIRE(base.columns() == 36864);
    REQUIRE(base.broken() == 0);

    // The duplicate control first: `t_c00` is `s_c00` written twice, so a
    // readout that is not deterministic fails here rather than somewhere it
    // would read as a disagreement with the server.
    const Readback duplicate(world, root / "t_c00");
    const Score same = equivariance(duplicate, base, 0, true);
    CHECK(same.hits == 36864);
    CHECK(same.total == 36864);

    struct Shift {
        int by;
        bool alongX;
        long long hits;
        long long total;
    };

    // Measured, both axes. The multiples of 16 are total; nothing else comes
    // near, and a per-column read would have made every row total.
    const std::vector<Shift> expected{
        {1, true, 7489, 36672},   {2, true, 4541, 36480},   {3, true, 3182, 36288},
        {4, true, 2533, 36096},   {5, true, 1671, 35904},   {6, true, 1442, 35712},
        {7, true, 783, 35520},    {8, true, 998, 35328},    {12, true, 1468, 34560},
        {16, true, 33792, 33792}, {32, true, 30720, 30720}, {1, false, 6526, 36672},
        {2, false, 4108, 36480},  {4, false, 2150, 36096},  {8, false, 3004, 35328},
        {16, false, 33792, 33792}};
    for (const auto& row : expected) {
        char name[8];
        std::snprintf(name, sizeof(name), "%c_c%02d", row.alongX ? 's' : 't', row.by);
        const Readback shifted(world, root / name);
        const Score got = equivariance(shifted, base, row.by, row.alongX);
        INFO(name << ": " << got.hits << " of " << got.total);
        CHECK(got.total == row.total);
        CHECK(got.hits == row.hits);
        // The statement, rather than the individual counts: a shift by a
        // multiple of the pitch is invisible, and every other shift is not.
        CHECK((row.by % 16 == 0) == (got.hits == got.total));
    }
}

TEST_CASE("the same pitch at a second seed", "[conformance][surface]") {
    // A per-column field fitted at one seed is exactly the kind of thing that
    // matches by luck, so the decisive rows run again at 31337.
    const std::filesystem::path root = fixtures() / "probes" / "psllat2";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat2 probe at " << root);
    }
    const World world{31337};
    const Readback base(world, root / "s_c00");
    REQUIRE(base.columns() == 36864);

    const std::vector<std::pair<int, std::pair<long long, long long>>> expected{
        {1, {7651, 36672}},
        {2, {4500, 36480}},
        {4, {2183, 36096}},
        {8, {1130, 35328}},
        {16, {33792, 33792}}};
    for (const auto& [by, counts] : expected) {
        char name[8];
        std::snprintf(name, sizeof(name), "s_c%02d", by);
        const Readback shifted(world, root / name);
        const Score got = equivariance(shifted, base, by, true);
        INFO(name << " at seed 31337: " << got.hits << " of " << got.total);
        CHECK(got.total == counts.second);
        CHECK(got.hits == counts.first);
        CHECK((by % 16 == 0) == (got.hits == got.total));
    }
}

TEST_CASE("the lattice is anchored at the world origin and nowhere else",
          "[conformance][surface]") {
    // The ANCHOR: one scan over one parameter at the pitch the case above
    // measured, rather than a corner of a joint fit. Exactly one of the 256
    // phases reproduces every column.
    const std::filesystem::path root = fixtures() / "probes" / "psllat";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat probe at " << root);
    }
    const World world{42};
    const auto fields = readSpec(root);
    const auto probe = probeNoise(root);
    const Readback back(world, root / "s_c00");
    const Field& field = fields.at("s_c00");

    int perfect = 0;
    long long runnerUp = 0;
    for (std::int32_t anchorZ = 0; anchorZ < kPitch; ++anchorZ) {
        for (std::int32_t anchorX = 0; anchorX < kPitch; ++anchorX) {
            const Score got = score(back, field, probe, Model::Measured, anchorX, anchorZ);
            if (anchorX == 0 && anchorZ == 0) {
                INFO("the world origin: " << got.hits << " of " << got.total);
                CHECK(got.hits == 36864);
                CHECK(got.total == 36864);
                continue;
            }
            runnerUp = std::max(runnerUp, got.hits);
            perfect += static_cast<int>(got.hits == got.total);
        }
    }
    INFO("255 other phases, the best of them " << runnerUp << " of 36864");
    // Not "the origin wins" but "nothing else comes close": the runner-up
    // reproduces a fifth of the columns.
    CHECK(perfect == 0);
    CHECK(runnerUp == 7542);
}

TEST_CASE("the four samples are blended linearly and the floor falls twice",
          "[conformance][surface]") {
    // The `f_*` dimensions are the ONLY probe of this entry whose arms are
    // not integers, and therefore the only ones that can separate where the
    // double becomes an int. With arms -0.5 / +0.5 and -0.25 / +0.75:
    //
    //   floor at the sample, blend, floor again   36864 of 36864  (shipped)
    //   blend the raw samples, floor once         20423 / 12121
    //   truncate at the sample                     3119 /  3119
    //   floor at the sample, truncate after        4764 /  4764
    //   floor at the sample, round after          22010 / 22010
    //   quantise to the cell's LOWER corner       21039 / 21039
    //   quantise to the NEAREST of the four       21103 / 21103
    //
    // Every earlier probe of this entry used integer arms, where the first
    // two readings are the same function — which is why this was open.
    //
    // WHAT THE TWO ROWS ARE AND ARE NOT. `f_half` and `f_quart` differ only
    // in their arms, and those arms land on the SAME side of every integer
    // the server can return: the case below measures that the two dimensions
    // carry byte-for-byte identical server data. So `f_quart` is not a second
    // observation of the floor placement — it is the same observation, and it
    // earns its place only by separating the REJECTED models from each other
    // (blend-then-floor scores 20423 against 12121 across the two). The floor
    // placement itself is measured at seed 42 here, at seed 31337 in
    // "the floor placement, at a second seed and in a second window", and
    // below zero in "the lattice cell is found with floorDiv".
    const std::filesystem::path root = fixtures() / "probes" / "psllat";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat probe at " << root);
    }
    const World world{42};
    const auto fields = readSpec(root);
    const auto probe = probeNoise(root);

    struct Row {
        const char* entry;
        long long floorAfter;
        long long truncBefore;
        long long truncAfter;
        long long roundAfter;
        long long lowerCorner;
        long long nearestCorner;
    };

    for (const Row& row : std::vector<Row>{{"f_half", 20423, 3119, 4764, 22010, 21039, 21103},
                                           {"f_quart", 12121, 3119, 4764, 22010, 21039, 21103}}) {
        const Readback back(world, root / row.entry);
        const Field& field = fields.at(row.entry);
        const Score measured = score(back, field, probe, Model::Measured);
        INFO(row.entry << ": measured " << measured.hits << " of " << measured.total);
        CHECK(measured.total == 36864);
        CHECK(measured.hits == 36864);
        CHECK(score(back, field, probe, Model::FloorAfter).hits == row.floorAfter);
        CHECK(score(back, field, probe, Model::TruncBefore).hits == row.truncBefore);
        CHECK(score(back, field, probe, Model::TruncAfter).hits == row.truncAfter);
        CHECK(score(back, field, probe, Model::RoundAfter).hits == row.roundAfter);
        CHECK(score(back, field, probe, Model::LowerCorner).hits == row.lowerCorner);
        CHECK(score(back, field, probe, Model::NearestCorner).hits == row.nearestCorner);
    }

    // And the pitch is not a property of the one field that measured it: the
    // same lattice reproduces every column at xz_scale 2 and 0.5, where the
    // driving noise's own features are half and twice the size.
    for (const char* entry : {"g_s2", "g_s05", "a_3arm", "s_c07"}) {
        const Readback back(world, root / entry);
        const Score got = score(back, fields.at(entry), probe, Model::Measured);
        INFO(entry << ": " << got.hits << " of " << got.total);
        CHECK(got.total == 36864);
        CHECK(got.hits == 36864);
    }
}

TEST_CASE("f_quart is the same server data as f_half, not a second measurement",
          "[conformance][surface]") {
    // The case above quotes two columns of counts, and it would be easy to
    // read them as two independent observations of where the floor falls.
    // They are not. `f_half` (arms -0.5 / +0.5) and `f_quart` (-0.25 / +0.75)
    // put both of their arms in the same unit interval, so `floor` sends both
    // pairs to the same two integers and the SERVER writes the same band in
    // both dimensions. Measured here rather than argued: the two readbacks
    // agree on every column, and the histogram of the recovered psl is the
    // same two values with the same counts.
    //
    // What `f_quart` does buy is the separation of the REJECTED models from
    // one another — "blend the raw samples, floor once" predicts a different
    // set of columns for arms a quarter of the way along a ramp than for arms
    // halfway along it, and that is the one column of the table above where
    // the two rows differ.
    const std::filesystem::path root = fixtures() / "probes" / "psllat";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat probe at " << root);
    }
    const World world{42};
    const Readback half(world, root / "f_half");
    const Readback quart(world, root / "f_quart");

    long long both = 0;
    long long same = 0;
    std::map<std::int32_t, long long> halfHistogram;
    std::map<std::int32_t, long long> quartHistogram;
    for (std::int32_t z = 0; z < Readback::kSide; ++z) {
        for (std::int32_t x = 0; x < Readback::kSide; ++x) {
            if (!half.has(x, z) || !quart.has(x, z)) {
                continue;
            }
            ++both;
            same += static_cast<long long>(half.at(x, z) == quart.at(x, z));
            ++halfHistogram[half.at(x, z)];
            ++quartHistogram[quart.at(x, z)];
        }
    }
    INFO("f_half vs f_quart: " << same << " of " << both << " columns identical");
    CHECK(both == 36864);
    CHECK(same == 36864);
    CHECK(halfHistogram == quartHistogram);
    // The two integers the arms floor to, with their own denominators, so a
    // reader can see that the pair is degenerate rather than take it on
    // trust.
    REQUIRE(halfHistogram.size() == 2);
    CHECK(halfHistogram[-1] == 33745);
    CHECK(halfHistogram[0] == 3119);
    CHECK(halfHistogram[-1] + halfHistogram[0] == 36864);
}

TEST_CASE("the floor placement, at a second seed and in a second window",
          "[conformance][surface]") {
    // So the double floor is not measured at one seed in one 192-block
    // window. `psllat2` carries `f_half` at seed 31337, on its own 36864
    // columns, and `psllat3` carries it at chunk -12 (scored in "the lattice
    // cell is found with floorDiv"), so the placement is measured at two
    // seeds and on both signs of the origin.
    const std::filesystem::path root = fixtures() / "probes" / "psllat2";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat2 probe at " << root);
    }
    const World world{31337};
    const auto fields = readSpec(root);
    const auto probe = probeNoise(root);
    const Readback back(world, root / "f_half");
    const Field& field = fields.at("f_half");

    const Score measured = score(back, field, probe, Model::Measured);
    INFO("f_half at seed 31337: " << measured.hits << " of " << measured.total);
    CHECK(measured.total == 36864);
    CHECK(measured.hits == 36864);
    CHECK(back.broken() == 0);
    // The same six refusals, at the second seed and with their own
    // denominators. The counts are NOT expected to match seed 42's — the
    // driving noise is a different field — only the verdict is.
    CHECK(score(back, field, probe, Model::FloorAfter).hits == 20164);
    CHECK(score(back, field, probe, Model::TruncBefore).hits == 2488);
    CHECK(score(back, field, probe, Model::TruncAfter).hits == 5409);
    CHECK(score(back, field, probe, Model::RoundAfter).hits == 21465);
    CHECK(score(back, field, probe, Model::LowerCorner).hits == 20664);
    CHECK(score(back, field, probe, Model::NearestCorner).hits == 20920);
}

TEST_CASE("the lattice cell is found with floorDiv, measured below zero",
          "[conformance][surface]") {
    // floorDiv and a truncating division are the SAME function while
    // x, z >= 0, so every probe this project has ever run on this entry was
    // blind to the difference. `probes/psllat3` is the same subset
    // FORCELOADED at chunk -12 — blocks -192..-65 — and the readback covers
    // x, z in [-256, -1], 65536 columns: the server generates a border of
    // chunks around the forceloaded square, and here that border lies
    // entirely inside r.-1.-1 and so is read back with it.
    const std::filesystem::path root = fixtures() / "probes" / "psllat3";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no psllat3 probe at " << root
                                    << "; generate it with tools/analysis/psl-lattice-probe.sh "
                                       "--accept-eula");
    }
    const World world{42};
    const auto fields = readSpec(root);
    const auto probe = probeNoise(root);

    const Readback base(world, root / "s_c00");
    REQUIRE(base.columns() == 65536);
    REQUIRE(base.broken() == 0);
    REQUIRE(base.baseX() == -512);
    REQUIRE(base.baseZ() == -512);

    const Score measured = score(base, fields.at("s_c00"), probe, Model::Measured);
    INFO("floorDiv: " << measured.hits << " of " << measured.total);
    CHECK(measured.total == 65536);
    CHECK(measured.hits == 65536);
    // The refutation, with its own denominator. On the positive side this
    // number is 65536 too; below zero the lattice would fold at x = 0.
    CHECK(score(base, fields.at("s_c00"), probe, Model::TruncatingCell).hits == 4216);

    // The floor placement holds below zero as well, on its own 65536 columns.
    const Readback half(world, root / "f_half");
    CHECK(score(half, fields.at("f_half"), probe, Model::Measured).hits == 65536);
    CHECK(score(half, fields.at("f_half"), probe, Model::FloorAfter).hits == 34387);

    // And the pitch, by translation, at negative coordinates.
    for (const auto& [by, hits] : std::vector<std::pair<int, bool>>{
             {1, false}, {2, false}, {4, false}, {8, false}, {16, true}}) {
        char name[8];
        std::snprintf(name, sizeof(name), "s_c%02d", by);
        const Readback shifted(world, root / name);
        const Score got = equivariance(shifted, base, by, true);
        INFO(name << " below zero: " << got.hits << " of " << got.total);
        CHECK((got.hits == got.total) == hits);
    }
}

TEST_CASE("neither the cell geometry nor flat_cache moves the lattice", "[conformance][surface]") {
    // The claim that this is not the CELL lattice, with the denominator it
    // was missing: `probes/apsb4` varies cell width 4/8/16 and cell height
    // with the varying field in place — `probes/apsb3` varied the same knobs
    // with a CONSTANT psl, where an interpolation is a no-op and nothing
    // could have shown. `w_flat` wraps the entry in `flat_cache`, which
    // relocates its argument to the 4x4 column corner: it changes nothing,
    // which is consistent with a lattice whose own samples are already taken
    // at multiples of 16.
    const std::filesystem::path root = fixtures() / "probes" / "apsb4";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no apsb4 probe at " << root);
    }
    const World world{42};
    const Readback base(world, root / "w_sh1");
    REQUIRE(base.columns() == 36864);
    for (const char* entry : {"w_sh2", "w_sh4", "w_sv4", "w_flat"}) {
        const Readback other(world, root / entry);
        long long both = 0;
        long long same = 0;
        for (std::int32_t z = 0; z < 512; ++z) {
            for (std::int32_t x = 0; x < 512; ++x) {
                if (!base.has(x, z) || !other.has(x, z)) {
                    continue;
                }
                ++both;
                same += static_cast<long long>(base.at(x, z) == other.at(x, z));
            }
        }
        INFO("w_sh1 vs " << entry << ": " << same << " of " << both);
        CHECK(both == 36864);
        CHECK(same == 36864);
    }
}

TEST_CASE("every dimension of the sweep, scored twice and with its extent measured",
          "[conformance][surface]") {
    // THREE things this case exists for, all of them denominators that were
    // being quoted from analyser runs rather than held by a test.
    //
    // ONE: the EXTENT of each probe, measured instead of restated. The
    // forceloaded square is 8x8 chunks — 128 blocks a side — but the server
    // generates a border of chunks around it and the readback reads whatever
    // the copied region file carries. What comes back is therefore neither
    // the square nor the region: `psllat` and `psllat2` carry x, z in
    // [0, 191] (36864 columns, the negative half of the border lying in
    // r.-1.-1, which is not copied) and `psllat3`, forceloaded at chunk -12,
    // carries x, z in [-256, -1] (65536 columns, its whole border inside
    // r.-1.-1).
    //
    // TWO: the sweep is scored over EVERY dimension it produced, not over the
    // handful the individual cases above reach for. 43 dimensions across the
    // three specs, each of them exact.
    //
    // THREE: the MODEL-FREE reading, which is the strongest form of the
    // claim available here. `cornerConsistency` predicts every interior
    // column from the SERVER's own recovered psl at the four multiples of 16
    // around it — no probe-noise replica, no anchor, and no calibration of
    // the first floor, since a recovered corner is already an integer. Only
    // the blend itself is under test, and it is under test on a population
    // that includes dimensions no replica-based case scores.
    struct Spec {
        const char* name;
        std::int64_t seed;
        int dimensions;
        long long columns;
        std::int32_t low;
        std::int32_t high;
        long long interior;
    };

    long long scoredColumns = 0;
    long long scoredInterior = 0;
    int scoredDimensions = 0;
    for (const Spec& spec : std::vector<Spec>{{"psllat", 42, 24, 36864, 0, 191, 30976},
                                              {"psllat2", 31337, 9, 36864, 0, 191, 30976},
                                              {"psllat3", 42, 10, 65536, -256, -1, 57600}}) {
        const std::filesystem::path root = fixtures() / "probes" / spec.name;
        if (!haveWorldgen() || !haveProbe(root)) {
            SKIP("no " << spec.name << " probe at " << root);
        }
        const World world{spec.seed};
        const auto fields = readSpec(root);
        const auto probe = probeNoise(root);
        INFO(spec.name << ": " << fields.size() << " dimensions carrying the entry");
        CHECK(static_cast<int>(fields.size()) == spec.dimensions);
        for (const auto& [name, field] : fields) {
            const Readback back(world, root / name);
            const Extent extent = extentOf(back);
            INFO(spec.name << '/' << name << ": " << extent.columns << " columns, x in ["
                           << extent.minX << ", " << extent.maxX << "], z in [" << extent.minZ
                           << ", " << extent.maxZ << ']');
            CHECK(back.broken() == 0);
            CHECK(extent.columns == spec.columns);
            CHECK(extent.minX == spec.low);
            CHECK(extent.maxX == spec.high);
            CHECK(extent.minZ == spec.low);
            CHECK(extent.maxZ == spec.high);

            const Score replica = score(back, field, probe, Model::Measured);
            INFO("against the replica: " << replica.hits << " of " << replica.total);
            CHECK(replica.total == spec.columns);
            CHECK(replica.hits == spec.columns);

            const Score modelFree = cornerConsistency(back);
            INFO("against the server's own corners: " << modelFree.hits << " of "
                                                      << modelFree.total);
            CHECK(modelFree.total == spec.interior);
            CHECK(modelFree.hits == spec.interior);

            scoredColumns += replica.total;
            scoredInterior += modelFree.total;
            ++scoredDimensions;
        }
    }
    INFO(scoredDimensions << " dimensions, " << scoredColumns << " columns against the replica, "
                          << scoredInterior << " interior columns model-free");
    CHECK(scoredDimensions == 43);
    CHECK(scoredColumns == 1871872);
    CHECK(scoredInterior == 1598208);
}

TEST_CASE("the model-free reading holds where the cell geometry varies too",
          "[conformance][surface]") {
    // `apsb4` is the family that varies cell width and height and wraps the
    // entry in `flat_cache`, and the case above it scores those dimensions
    // only against EACH OTHER. The model-free reading can score them
    // outright, because it needs nothing from the dimension but its own
    // readback — which is what makes it worth having separately from the
    // replica: a dimension whose geometry the replica does not model is still
    // scored here.
    const std::filesystem::path root = fixtures() / "probes" / "apsb4";
    if (!haveWorldgen() || !haveProbe(root)) {
        SKIP("no apsb4 probe at " << root);
    }
    const World world{42};
    const auto fields = readSpec(root);
    CHECK(fields.size() == 6);

    long long total = 0;
    for (const auto& entry : fields) {
        const std::string& name = entry.first;
        const Readback back(world, root / name);
        const Score got = cornerConsistency(back);
        INFO("apsb4/" << name << ": " << got.hits << " of " << got.total);
        CHECK(got.total == 30976);
        CHECK(got.hits == 30976);
        total += got.total;
    }
    CHECK(total == 185856);
}

TEST_CASE("above_preliminary_surface occurs exactly three times in the pinned tree",
          "[conformance][surface]") {
    // The universal the whole golden argument rests on, and until now only a
    // comment: `above_preliminary_surface` gates the overworld's
    // surface-materials subtree and appears NOWHERE ELSE, so a block that
    // subtree alone can place is a block that condition alone can gate. If a
    // future pin grew a fourth use — in a carver's rule, in the nether — the
    // golden cases below would keep passing while the sentence they rest on
    // quietly stopped being true. This is the assertion that would fail
    // instead.
    if (!haveWorldgen()) {
        SKIP("no worldgen tree at " << (fixtures() / "worldgen"));
    }
    // Counted as JSON NODES rather than as text, so a mention inside a string
    // or a comment could not pass for a use of the condition.
    const std::function<long long(const nlohmann::json&)> count =
        [&count](const nlohmann::json& node) -> long long {
        long long found = 0;
        if (node.is_object()) {
            const auto type = node.find("type");
            if (type != node.end() && type->is_string() &&
                type->get<std::string>() == "minecraft:above_preliminary_surface") {
                ++found;
            }
        }
        if (node.is_object() || node.is_array()) {
            for (const auto& child : node) {
                found += count(child);
            }
        }
        return found;
    };

    std::map<std::string, long long> uses;
    long long total = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(fixtures() / "worldgen")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const long long found = count(nlohmann::json::parse(std::ifstream(entry.path())));
        if (found == 0) {
            continue;
        }
        uses[std::filesystem::relative(entry.path(), fixtures() / "worldgen").generic_string()] =
            found;
        total += found;
    }

    for (const auto& [file, found] : uses) {
        INFO(file << ": " << found);
        CHECK(found == 1);
    }
    CHECK(total == 3);
    const std::map<std::string, long long> expected{{"noise_settings/amplified.json", 1},
                                                    {"noise_settings/large_biomes.json", 1},
                                                    {"noise_settings/overworld.json", 1}};
    CHECK(uses == expected);
}

TEST_CASE("the lattice on real terrain, both directions and every grass block",
          "[conformance][surface]") {
    // REAL TERRAIN, WHERE THIS COULD HAVE SAID NO — one of two such tests,
    // not the only one, and run in both directions because either direction
    // alone is weak in a way the other is not.
    // `vanilla_above_preliminary_surface_test.cpp`'s "real overworlds, both
    // directions of the boundary at once" runs the same two directions under
    // the PER-COLUMN reading; this is that case's twin under the lattice, and
    // the per-column figures are recomputed here in the same pass so the two
    // readings are compared on identical populations rather than across
    // files.
    //
    // FORWARD — a block the reading cannot place. `above_preliminary_surface`
    // gates the overworld's surface-materials subtree and occurs exactly
    // three times in the whole pinned tree, all of them that same gate (the
    // case above asserts it), so a `grass_block` BELOW the boundary is a
    // block the reading cannot produce at all. The population is EVERY
    // grass_block in the eight regions — not, as the earlier version of this
    // case had it, only those below the per-column psl. That subset cannot
    // expose a lattice psl that sits HIGHER than the per-column one, which is
    // exactly the direction in which a wrong blend would fail; the full
    // population can.
    //
    // REVERSE — the band the reading OPENS. Counting only what the old
    // reading cannot explain rewards a boundary for reaching further down: a
    // boundary at the world floor would score perfectly forward. So the band
    // `[psl + depth - 8, psl)` is also read at the one place vanilla's own
    // behaviour is pinned — the column's SURFACE, the first block from the
    // sky that is neither air nor fluid — where the materials subtree is what
    // decides the block. The lattice opens a DIFFERENT band from the per-
    // column reading, so this is a different population, and both are scored.
    //
    // Absence of a gated-only material is never counted as a refutation here:
    // the subtree is a `sequence` whose inner rules may decline at a position
    // its gate opened. That makes the reverse direction a bound, and it says
    // so.
    if (!haveWorldgen()) {
        SKIP("no worldgen tree at " << (fixtures() / "worldgen"));
    }

    struct Seed {
        std::int64_t seed;
        long long grass;       // every grass_block in the region
        long long belowOld;    // ... below the per-column psl
        long long perColumn;   // ... of those, inside the per-column band
        long long surfaceBand; // column surfaces inside the LATTICE band
        long long surfaceGated;
    };

    const std::vector<Seed> seeds{
        {0, 168678, 2490, 2231, 1020, 1012},
        {-1, 32288, 921, 865, 1685, 1620},
        {1, 141473, 2492, 2207, 1643, 1544},
        // Desert and ocean end to end: no grass_block anywhere in the region,
        // so the forward direction is silent here and the reverse one is not.
        // That asymmetry is most of why both directions are run.
        {2891948927356891, 0, 0, 0, 1015, 1015},
        {-4172144997902289642, 883, 843, 842, 1550, 1543},
        {42, 16458, 781, 755, 699, 695},
        {9223372036854775807, 48189, 1593, 1189, 3588, 3295},
        {std::numeric_limits<std::int64_t>::min(), 219797, 4888, 3490, 1716, 1679}};

    long long grass = 0;
    long long belowOld = 0;
    long long perColumn = 0;
    long long lattice = 0;
    long long belowLattice = 0;
    long long surfaceBandColumn = 0;
    long long surfaceGatedColumn = 0;
    long long surfaceBandLattice = 0;
    long long surfaceGatedLattice = 0;
    std::map<std::string, long long> latticeSurfaceBlocks;
    int scored = 0;
    for (const auto& row : seeds) {
        const std::filesystem::path region = fixtures() / "regions" /
                                             ("seed-" + std::to_string(row.seed)) / "overworld" /
                                             "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const World world{row.seed};
        const auto file = stratum::region::RegionFile::open(region);
        long long seedGrass = 0;
        long long seedBelow = 0;
        long long seedColumn = 0;
        long long seedLattice = 0;
        long long seedBelowLattice = 0;
        long long seedSurfaceBand = 0;
        long long seedSurfaceGated = 0;
        for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto chunk = stratum::chunk::Chunk::decode(
                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        const std::int32_t column = world.pslPerColumn(x, z);
                        const std::int32_t blended = world.pslLattice(x, z);
                        const std::int32_t depth = world.surfaceDepth(x, z);

                        // Forward, over EVERY grass_block in the column.
                        for (std::int32_t y = world.minY(); y < world.topY(); ++y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr || found->name != "minecraft:grass_block") {
                                continue;
                            }
                            ++seedGrass;
                            // The one that would be a counter-example: a
                            // grass_block the LATTICE band cannot reach.
                            seedBelowLattice += static_cast<long long>(y < blended + depth - 8);
                            if (y >= column) {
                                continue;
                            }
                            ++seedBelow;
                            seedColumn += static_cast<long long>(y >= column + depth - 8);
                            seedLattice += static_cast<long long>(y >= blended + depth - 8);
                        }

                        // Reverse: the column's own surface, under the band
                        // the LATTICE opens.
                        for (std::int32_t y = world.topY() - 1; y >= world.minY(); --y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr) {
                                continue;
                            }
                            const std::string& name = found->name;
                            if (name == "minecraft:air" || name == "minecraft:cave_air" ||
                                name == "minecraft:water" || name == "minecraft:lava") {
                                continue;
                            }
                            if (y >= blended + depth - 8 && y < blended) {
                                ++seedSurfaceBand;
                                ++latticeSurfaceBlocks[name];
                                seedSurfaceGated += static_cast<long long>(gatedOnlyMaterial(name));
                            }
                            // The per-column band, recomputed in the same
                            // pass so the comparison is like for like.
                            if (y >= column + depth - 8 && y < column) {
                                ++surfaceBandColumn;
                                surfaceGatedColumn +=
                                    static_cast<long long>(gatedOnlyMaterial(name));
                            }
                            break;
                        }
                    }
                }
            }
        }
        INFO("seed " << row.seed << ": " << seedGrass << " grass_blocks, " << seedBelow
                     << " below the old boundary, " << seedColumn << " inside the per-column band, "
                     << seedLattice << " inside the lattice's, " << seedBelowLattice
                     << " below the lattice band; " << seedSurfaceBand
                     << " column surfaces inside the lattice band, " << seedSurfaceGated
                     << " of them gated-only");
        CHECK(seedGrass == row.grass);
        CHECK(seedBelow == row.belowOld);
        CHECK(seedColumn == row.perColumn);
        // Per seed rather than pooled: a residual that closed on seven seeds
        // and doubled on the eighth would pool to something respectable.
        CHECK(seedLattice == seedBelow);
        // The counter-example that never appears, per seed and over the whole
        // grass population rather than over the subset the per-column reading
        // already picked out.
        CHECK(seedBelowLattice == 0);
        CHECK(seedSurfaceBand == row.surfaceBand);
        CHECK(seedSurfaceGated == row.surfaceGated);
        grass += seedGrass;
        belowOld += seedBelow;
        perColumn += seedColumn;
        lattice += seedLattice;
        belowLattice += seedBelowLattice;
        surfaceBandLattice += seedSurfaceBand;
        surfaceGatedLattice += seedSurfaceGated;
        ++scored;
    }
    if (scored == 0) {
        SKIP("no golden overworld regions under " << (fixtures() / "regions"));
    }
    REQUIRE(scored == 8);

    INFO(grass << " grass_blocks in all, " << belowOld << " below the old boundary, " << perColumn
               << " explained per column, " << lattice << " explained by the lattice, "
               << belowLattice << " below the lattice band");
    CHECK(grass == 627766);
    CHECK(belowOld == 14008);
    CHECK(perColumn == 11579);
    CHECK(lattice == 14008);
    // The assertion FIX-worthy on its own: over every grass_block the server
    // wrote in the eight regions, not one falls below the band the lattice
    // opens.
    CHECK(belowLattice == 0);

    INFO(surfaceBandLattice << " column surfaces inside the lattice band, " << surfaceGatedLattice
                            << " of them a block only the gated subtree places; per column "
                            << surfaceGatedColumn << " of " << surfaceBandColumn);
    // The reverse direction under the lattice, and the per-column baseline it
    // is measured against. The baseline is the one
    // `vanilla_above_preliminary_surface_test.cpp` asserts, recomputed here,
    // so a disagreement between the two files is a failure rather than a
    // discrepancy nobody notices.
    CHECK(surfaceBandColumn == 11953);
    CHECK(surfaceGatedColumn == 10676);
    CHECK(surfaceBandLattice == 12916);
    CHECK(surfaceGatedLattice == 12403);
    // Named rather than dropped into a residual, the same way the per-column
    // case names its 1277: every surface in the lattice band that is not a
    // gated-only material is `stone` — which the subtree itself places, so
    // the block is silent about whether the subtree ran — or a feature's own
    // block written after the surface pass.
    std::map<std::string, long long> unattributed;
    long long unattributable = 0;
    for (const auto& [name, count] : latticeSurfaceBlocks) {
        if (gatedOnlyMaterial(name)) {
            continue;
        }
        INFO("not a gated-only material: " << name << " x" << count);
        CHECK((name == "minecraft:stone" || name == "minecraft:granite" ||
               name == "minecraft:copper_ore"));
        unattributed[name] = count;
        unattributable += count;
    }
    // Spelled out per block rather than as one residual, so "the rest is
    // stone and a few feature blocks" is a measurement and not a
    // reassurance.
    const std::map<std::string, long long> expected{
        {"minecraft:copper_ore", 1}, {"minecraft:granite", 9}, {"minecraft:stone", 503}};
    CHECK(unattributed == expected);
    CHECK(unattributable == 513);
    CHECK(surfaceGatedLattice + unattributable == surfaceBandLattice);
}
