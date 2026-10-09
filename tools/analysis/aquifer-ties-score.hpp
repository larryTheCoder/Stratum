// Stratum — scores aquifer-ties-rivals.hpp's readings on a probe dimension.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// One aquifer dimension of a density-probe.sh corpus, loaded the way the
// filler loads it (its spec entry as noise settings, the probe's noise, and
// vanilla's `aquifer_barrier` from the fetched worldgen tree), and every
// reading scored through `computeSubstanceWith` on the chunks the probe
// generated without ticking them (support/probe_region.hpp) — or, with no
// region, against the shipped reading alone, which is what a design is
// checked with before a server runs. Shared by
// tools/analysis/aquifer-ties-analyze.cpp and
// tests/conformance/vanilla_aquifer_ties_test.cpp.
#pragma once

#include "aquifer-ties-rivals.hpp"
#include "support/fluid_flow.hpp"
#include "support/probe_region.hpp"
#include "support/probe_settings.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace stratum::analysis::ties {

/// density-probe.sh's own noise, as its manifest records it.
[[nodiscard]] inline nlohmann::json probeNoise() {
    return {{"firstOctave", -3}, {"amplitudes", {1.0}}};
}

/// One probe dimension as a data pack on disk, removed when it goes out of
/// scope; nothing here is committed.
class ProbePack {
public:
    ProbePack(const nlohmann::json& entry, const nlohmann::json& noise,
              const std::filesystem::path& worldgen)
        : path_(test::tempPath("stratum-ties")), name_(entry.at("name").get<std::string>()) {
        std::filesystem::create_directories(path_);
        std::ofstream(path_ / "pack.mcmeta") << R"({"pack":{"description":"stratum ties replay",)"
                                                R"("min_format":[94,1],"max_format":94}})";
        // The library refuses an aquifer whose default fluid is not water
        // (pack validation): packed ice is replayed as water, which the
        // aquifer places in the same positions (aquifer-level-probe.sh's
        // water twin), and the scorer reads the server's ice as that fluid.
        // What is not the same is Q6.3 on row lambda, which `scoreReadings`
        // leaves out of an ice world.
        nlohmann::json settings = test::probeNoiseSettings(entry);
        if (settings.at("default_fluid").at("Name") == "minecraft:packed_ice") {
            settings["default_fluid"] = {{"Name", "minecraft:water"},
                                         {"Properties", {{"level", "0"}}}};
        }
        write("stratum", "noise_settings", name_, settings);
        write("stratum", "noise", "probe_noise", noise);
        write("minecraft", "noise", "aquifer_barrier",
              nlohmann::json::parse(std::ifstream(worldgen / "noise" / "aquifer_barrier.json")));
    }

    ProbePack(const ProbePack&) = delete;
    ProbePack& operator=(const ProbePack&) = delete;
    ProbePack(ProbePack&&) = delete;
    ProbePack& operator=(ProbePack&&) = delete;

    ~ProbePack() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] data::Pack pack() const { return data::Pack::openDataPack(path_); }

    [[nodiscard]] data::ResourceLocation settingsId() const {
        return data::ResourceLocation{"stratum", name_};
    }

private:
    void write(const std::string& space, const std::string& registry, const std::string& name,
               const nlohmann::json& body) const {
        const std::filesystem::path file =
            path_ / "data" / space / "worldgen" / registry / (name + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << body.dump();
    }

    std::filesystem::path path_;
    std::string name_;
};

/// One aquifer dimension, loaded the way the filler loads it.
class Dimension {
public:
    Dimension(const nlohmann::json& entry, const nlohmann::json& noise,
              const std::filesystem::path& worldgen, const std::int64_t seed)
        : probe_(entry, noise, worldgen), pack_(probe_.pack()), loaded_(settings::loadAll(pack_)),
          settings_(loaded_.settings.at(probe_.settingsId())),
          noises_(density::NoiseRegistry::create(pack_, loaded_.graph.referencedNoises(), seed,
                                                 density::RandomSource::Xoroshiro)),
          interpreter_(loaded_.graph, noises_),
          filler_(terrain::ChunkFiller::compile(loaded_.graph, noises_, settings_)),
          centres_(seed, density::RandomSource::Xoroshiro),
          fluid_(entry.at("default_fluid").at("Name").get<std::string>()) {}

    [[nodiscard]] const settings::NoiseSettings& settings() const { return settings_; }

    [[nodiscard]] const terrain::ChunkFiller& filler() const { return filler_; }

    [[nodiscard]] const aquifer::CentreSource& centres() const { return centres_; }

    /// The default fluid's block name.
    [[nodiscard]] const std::string& fluid() const { return fluid_; }

    /// A router entry at a point, uncached (every entry here is constant in
    /// y or a flat function of the column).
    [[nodiscard]] double read(settings::RouterEntry entry, std::int32_t x, std::int32_t y,
                              std::int32_t z) const {
        return interpreter_.evaluate(settings_.router.at(entry),
                                     density::Point{.x = x, .y = y, .z = z});
    }

private:
    ProbePack probe_;
    data::Pack pack_;
    settings::LoadedSettings loaded_;
    const settings::NoiseSettings& settings_;
    density::NoiseRegistry noises_;
    density::Interpreter interpreter_;
    terrain::ChunkFiller filler_;
    aquifer::CentreSource centres_;
    std::string fluid_;
};

/// The region's chunks a probe can have generated: the 8x8 window, its
/// ticked ring, and the untouched band out to chunk 11.
inline constexpr std::int32_t kRegionChunks = 12;

/// A column an untouched chunk shares a face with a ticked one: the
/// neighbour's flow can reach it (vanilla_aquifer_yskip_test.cpp measured
/// that it reaches no further in).
[[nodiscard]] inline bool besideTicked(std::int32_t x, std::int32_t z) {
    constexpr std::int32_t kFace = 9 * 16;
    return (x == kFace && z < kFace) || (z == kFace && x < kFace);
}

[[nodiscard]] inline test::Category categoryOf(const aquifer::SubstanceAt& at) {
    switch (at.substance) {
        case aquifer::Substance::Air:
            return test::Category::Air;
        case aquifer::Substance::Solid:
            return test::Category::Solid;
        case aquifer::Substance::Fluid:
            return at.fluidType == aquifer::FluidType::Lava ? test::Category::Lava
                                                            : test::Category::Water;
    }
    return test::Category::Solid;
}

/// The server's block as a category, the default fluid (@p fluid, packed ice
/// or water) reading as Water; empty for a block generation does not write
/// in these worlds — fluid that moved, or what fluid left behind.
[[nodiscard]] inline std::optional<test::Category> servedOf(const chunk::BlockState* block,
                                                            const std::string& fluid) {
    if (block == nullptr || block->name == "minecraft:air") {
        return test::Category::Air;
    }
    if (block->name == "minecraft:stone") {
        return test::Category::Solid;
    }
    if (block->name == fluid && (fluid != "minecraft:water" || test::fluidLevel(block) == 0)) {
        return test::Category::Water;
    }
    if (block->name == "minecraft:lava" && test::fluidLevel(block) == 0) {
        return test::Category::Lava;
    }
    return std::nullopt;
}

/// Everything one dimension's untouched chunks say about the readings.
struct Score {
    int chunks = 0;
    long long blocks = 0;      ///< blocks scored (with a region), or replayed (without)
    long long moved = 0;       ///< server blocks generation does not write, left out
    long long unexplained = 0; ///< of those, not shaped like flow
    long long marks = 0;       ///< server marks at the scored positions
    std::array<long long, kReadingCount> wrongBlocks{};
    std::array<long long, kReadingCount> wrongMarks{};
    /// Of the scored blocks, those where a reading's category differs from
    /// the shipped one: where the server can take a side.
    std::array<long long, kReadingCount> differsScored{};
    /// Positions where a reading's category differs from the shipped one,
    /// whatever the server holds.
    std::array<long long, kReadingCount> differsFromShipped{};
    /// Distinct sources ranked anywhere in the scored blocks, by case.
    std::array<long long, static_cast<std::size_t>(Case::Count)> sources{};
    /// Sources where the rebuilt scan, the sample-by-sample spec, or engine
    /// v12 on a source in no case disagrees with the library: must stay 0.
    long long drift = 0;
};

/// Whether a reading's FLUID result is one the server marks: the server
/// lists a position only where the block placed holds a fluid, which packed
/// ice does not.
[[nodiscard]] inline bool marked(const aquifer::SubstanceAt& at, const std::string& fluid) {
    if (at.substance != aquifer::Substance::Fluid || !at.fluidUpdate) {
        return false;
    }
    return at.fluidType == aquifer::FluidType::Lava || fluid == "minecraft:water";
}

/// Scores every reading on @p dimension's untouched chunks, every
/// @p stride-th column both ways, on the rows the lattice decides (lambda
/// up to the chunk's y_skip). With no @p region, only the model-against-
/// model tallies are filled.
inline Score scoreReadings(const Dimension& dimension,
                           const std::optional<std::filesystem::path>& region,
                           const std::int32_t stride,
                           const std::int32_t rowFrom = std::numeric_limits<std::int32_t>::min(),
                           const std::int32_t rowTo = std::numeric_limits<std::int32_t>::max()) {
    const settings::NoiseSettings& settings = dimension.settings();
    const std::int32_t sea = settings.seaLevel;
    const std::int32_t lambda = aquifer::lambdaLevel(sea);
    // Q6.3, water over the lava sea, fires on row lambda alone and is
    // measured only for water: an ice world's row lambda is not scored.
    const std::int32_t firstRow = dimension.fluid() == "minecraft:water" ? lambda : lambda + 1;
    const std::int32_t minY = settings.geometry.minY;
    const auto router = [&](settings::RouterEntry entry) {
        return [&dimension, entry](std::int32_t x, std::int32_t y, std::int32_t z) {
            return dimension.read(entry, x, y, z);
        };
    };
    const auto psl = router(settings::RouterEntry::PreliminarySurfaceLevel);
    const auto barrier = router(settings::RouterEntry::Barrier);
    const auto floodedness = router(settings::RouterEntry::FluidLevelFloodedness);
    const auto spread = router(settings::RouterEntry::FluidLevelSpread);
    const auto lava = router(settings::RouterEntry::Lava);
    const auto deepDark = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquifer::isDeepDark(dimension.read(settings::RouterEntry::Erosion, x, y, z),
                                   dimension.read(settings::RouterEntry::Depth, x, y, z));
    };
    using Key = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
    std::map<Key, Source> sources;
    Score score;
    const auto sourceFor = [&](const aquifer::Source& ranked) -> const Source& {
        const Key key{ranked.centre.x, ranked.centre.y, ranked.centre.z};
        auto found = sources.find(key);
        if (found == sources.end()) {
            const aquifer::RankedCell inputs =
                aquifer::rankedCellOf(ranked, sea, psl, floodedness, spread, lava, deepDark);
            found = sources.emplace(key, sourceOf(inputs, psl, ranked.centre)).first;
            const Source& made = found->second;
            ++score.sources.at(static_cast<std::size_t>(made.where));
            score.drift += made.readAgrees && made.specAgrees && made.sameAgrees ? 0 : 1;
        }
        return found->second;
    };

    std::optional<test::GoldenRegion> golden;
    std::optional<region::RegionFile> file;
    if (region.has_value()) {
        golden.emplace(*region);
        file.emplace(region::RegionFile::open(*region));
    }
    for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
            std::set<test::LocalPosition> marks;
            if (file.has_value()) {
                if (!file->hasChunk(cx, cz)) {
                    continue;
                }
                const auto doc = nbt::read(file->readChunk(cx, cz));
                if (!test::untouched(cx, cz, doc.root.at("Status").asString())) {
                    continue;
                }
                marks = test::postProcessingMarks(doc.root, minY);
            } else if (test::ticked(cx, cz)) {
                continue;
            }
            ++score.chunks;
            const std::int32_t ySkip = aquifer::chunkYSkip(psl, cx * 16, cz * 16);
            for (std::int32_t lz = 0; lz < 16; lz += stride) {
                for (std::int32_t lx = 0; lx < 16; lx += stride) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = std::max(firstRow, rowFrom); y <= std::min(ySkip, rowTo);
                         ++y) {
                        const aquifer::AquiferQuery query{
                            .x = x, .y = y, .z = z, .density = -1.0, .seaLevel = sea};
                        const aquifer::Selection selection =
                            aquifer::selectSources(dimension.centres(), x, y, z);
                        std::array<const Source*, aquifer::kRankCount> ranked{};
                        for (std::size_t r = 0; r < ranked.size(); ++r) {
                            ranked.at(r) = &sourceFor(selection.ranked.at(r));
                        }
                        std::optional<double> barrierHere;
                        const auto barrierOnce = [&](std::int32_t bx, std::int32_t by,
                                                     std::int32_t bz) {
                            if (!barrierHere.has_value()) {
                                barrierHere = barrier(bx, by, bz);
                            }
                            return *barrierHere;
                        };
                        std::array<aquifer::SubstanceAt, kReadingCount> out{};
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            if (r > 0 && std::ranges::all_of(ranked, [&](const Source* s) {
                                    return sameStatus(s->status.at(r), s->status[0]);
                                })) {
                                out.at(r) = out[0];
                                continue;
                            }
                            // The decision ranks the same four sources again;
                            // each is found among those already looked up.
                            out.at(r) = aquifer::computeSubstanceWith(
                                dimension.centres(), query,
                                [&](const aquifer::Source& source) {
                                    for (std::size_t i = 0; i < ranked.size(); ++i) {
                                        if (selection.ranked.at(i).centre == source.centre) {
                                            return ranked.at(i)->status.at(r);
                                        }
                                    }
                                    return sourceFor(source).status.at(r);
                                },
                                barrierOnce);
                        }
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            score.differsFromShipped.at(r) +=
                                categoryOf(out.at(r)) != categoryOf(out[0]) ? 1 : 0;
                        }
                        if (!golden.has_value()) {
                            ++score.blocks;
                            continue;
                        }
                        const bool isMarked = marks.contains(test::LocalPosition{lx, y, lz});
                        score.marks += isMarked ? 1 : 0;
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            score.wrongMarks.at(r) +=
                                marked(out.at(r), dimension.fluid()) == isMarked ? 0 : 1;
                        }
                        if (besideTicked(x, z)) {
                            continue; // a ticked neighbour's flow can reach the blocks here
                        }
                        const chunk::BlockState* block = golden->blockAt(x, y, z);
                        const std::optional<test::Category> served =
                            servedOf(block, dimension.fluid());
                        if (!served.has_value()) {
                            ++score.moved;
                            const test::Category raw = block == nullptr
                                                           ? test::Category::Air
                                                           : test::categoryOf(block->name);
                            score.unexplained +=
                                test::explainedByFlow(*golden, x, y, z, raw, categoryOf(out[0]))
                                    ? 0
                                    : 1;
                            continue;
                        }
                        ++score.blocks;
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            score.wrongBlocks.at(r) += categoryOf(out.at(r)) == *served ? 0 : 1;
                            score.differsScored.at(r) +=
                                categoryOf(out.at(r)) != categoryOf(out[0]) ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    return score;
}

} // namespace stratum::analysis::ties
