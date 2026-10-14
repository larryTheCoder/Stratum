// Stratum — the Nukkit binding's own generation pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/data/pack.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <stratum_nukkit/pipeline.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace stratum::nukkit {

namespace {

constexpr int kChunkWidth = 16;
constexpr int kQuartWidth = 4;
constexpr int kQuartsPerChunkEdge = 4;

[[nodiscard]] settings::BlockState named(std::string_view name) {
    return settings::BlockState{.name = data::ResourceLocation::parse(name), .properties = {}};
}

/// Appends @p state unless an equal one is already listed: the order stays
/// the order of first appearance, which keeps messages and tests stable.
void appendUnique(std::vector<settings::BlockState>& into, settings::BlockState state) {
    if (std::find(into.begin(), into.end(), state) == into.end()) {
        into.push_back(std::move(state));
    }
}

[[nodiscard]] std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw NukkitError("stratum-nukkit: cannot read the frozen pipeline at '" + path.string() +
                          "'");
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    if (!raw.empty()) {
        std::memcpy(bytes.data(), raw.data(), raw.size());
    }
    return bytes;
}

} // namespace

void freezePack(const std::filesystem::path& versionRoot, const std::filesystem::path& blobPath) {
    std::error_code error;
    if (std::filesystem::exists(blobPath, error)) {
        throw NukkitError("stratum-nukkit: refusing to replace the frozen pipeline at '" +
                          blobPath.string() +
                          "': a world's pipeline is written once, before it has a chunk, and "
                          "never again (SPEC §6)");
    }
    if (!std::filesystem::is_directory(versionRoot / "worldgen", error)) {
        throw NukkitError("stratum-nukkit: '" + versionRoot.string() +
                          "' is not a version root: it has no worldgen/ directory (point "
                          "\"pack\" at what tools/fetch-vanilla leaves, e.g. .fixtures/1.21.11)");
    }
    const std::vector<std::byte> blob = freeze::write(freeze::resolve(
        data::Pack::open(versionRoot / "worldgen"), versionRoot / "biome_parameters"));

    if (blobPath.has_parent_path()) {
        std::filesystem::create_directories(blobPath.parent_path());
    }
    std::filesystem::path partial = blobPath;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(partial, error);
            throw NukkitError("stratum-nukkit: cannot write the frozen pipeline to '" +
                              partial.string() + "'");
        }
    }
    std::filesystem::rename(partial, blobPath);
}

BlockIdTable BlockIdTable::fromResolved(const std::span<const std::int32_t> javaStateIds,
                                        const std::span<const std::int32_t> fullIds) {
    if (javaStateIds.size() != fullIds.size()) {
        throw NukkitError("stratum-nukkit: " + std::to_string(javaStateIds.size()) +
                          " resolved Java block states but " + std::to_string(fullIds.size()) +
                          " Nukkit ids for them");
    }
    const std::size_t count = mapping::javaBlockStateCount();
    BlockIdTable table;
    table.byJavaState_.assign(count, -1);
    for (std::size_t i = 0; i < javaStateIds.size(); ++i) {
        const std::int32_t javaId = javaStateIds[i];
        const std::int32_t fullId = fullIds[i];
        if (javaId < 0 || static_cast<std::size_t>(javaId) >= count) {
            throw NukkitError("stratum-nukkit: Java block state id " + std::to_string(javaId) +
                              " is outside the pinned version's " + std::to_string(count) +
                              " states");
        }
        if (fullId < 0) {
            throw NukkitError("stratum-nukkit: Java block state id " + std::to_string(javaId) +
                              " was given the negative Nukkit full id " + std::to_string(fullId));
        }
        std::int32_t& slot = table.byJavaState_[static_cast<std::size_t>(javaId)];
        if (slot >= 0 && slot != fullId) {
            throw NukkitError("stratum-nukkit: Java block state id " + std::to_string(javaId) +
                              " was resolved twice, to Nukkit full ids " + std::to_string(slot) +
                              " and " + std::to_string(fullId));
        }
        if (slot < 0) {
            ++table.resolved_;
        }
        slot = fullId;
    }
    return table;
}

std::optional<std::int32_t> BlockIdTable::find(const std::uint32_t javaStateId) const noexcept {
    if (javaStateId >= byJavaState_.size() || byJavaState_[javaStateId] < 0) {
        return std::nullopt;
    }
    return byJavaState_[javaStateId];
}

std::vector<settings::BlockState> emittableBlockStates(const settings::NoiseSettings& settings) {
    std::vector<settings::BlockState> states;
    // terrain::ChunkFiller: air above the terrain and in caves; lava, which
    // the aquifer and the aquifer-free fill both place literally at level
    // 0 whatever the default fluid is; the default block and fluid.
    appendUnique(states, named("minecraft:air"));
    appendUnique(states,
                 settings::BlockState{.name = data::ResourceLocation::parse("minecraft:lava"),
                                      .properties = {{"level", "0"}}});
    appendUnique(states, settings.defaultBlock);
    appendUnique(states, settings.defaultFluid);
    if (settings.oreVeinsEnabled) {
        for (const char* vein :
             {"minecraft:granite", "minecraft:copper_ore", "minecraft:raw_copper_block",
              "minecraft:tuff", "minecraft:deepslate_iron_ore", "minecraft:raw_iron_block"}) {
            appendUnique(states, named(vein));
        }
    }
    // surface::Executor: every `block` rule, and bandlands' terracottas.
    if (!settings.surfaceRule.is_null()) {
        const surface::RuleGraph rules =
            surface::RuleGraph::resolve(settings.surfaceRule, settings.id);
        bool bandlands = false;
        for (surface::RuleIndex index = 0; index < rules.ruleCount(); ++index) {
            const surface::Rule& rule = rules.rule(index);
            if (rule.type == surface::RuleType::Block) {
                appendUnique(states, rule.block);
            } else if (rule.type == surface::RuleType::Bandlands) {
                bandlands = true;
            }
        }
        if (bandlands) {
            for (const char* clay : {"minecraft:terracotta", "minecraft:orange_terracotta",
                                     "minecraft:yellow_terracotta", "minecraft:brown_terracotta",
                                     "minecraft:red_terracotta", "minecraft:white_terracotta",
                                     "minecraft:light_gray_terracotta"}) {
                appendUnique(states, named(clay));
            }
        }
    }
    return states;
}

std::string describe(const settings::BlockState& state) {
    std::string text = state.name.toString();
    if (!state.properties.empty()) {
        text += '[';
        bool first = true;
        for (const auto& [property, value] : state.properties) {
            if (!first) {
                text += ',';
            }
            first = false;
            text += property;
            text += '=';
            text += value;
        }
        text += ']';
    }
    return text;
}

struct Pipeline::Impl {
    std::unique_ptr<world::CompiledDimension> dimension;
    std::vector<settings::BlockState> emittableStates;
    std::vector<std::uint32_t> emittableStateIds;
    std::vector<Biome> emittableBiomes;
};

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

std::unique_ptr<Pipeline> Pipeline::compile(freeze::Pipeline pipeline,
                                            const data::ResourceLocation& settings,
                                            const world::BiomeSource& biomes,
                                            const std::int64_t worldSeed) {
    if (biomes.type() == world::BiomeSource::Type::TheEnd) {
        throw NukkitError("stratum-nukkit: the biome source '" + biomes.toString() +
                          "' is outside this binding's scope: Nukkit's End (y 0..255) has not "
                          "been matched to Stratum's yet, only its overworld");
    }

    auto result = std::unique_ptr<Pipeline>(new Pipeline());
    result->impl_ = std::make_unique<Impl>();
    Impl& impl = *result->impl_;

    // Read before the pipeline moves into the compiled dimension. A name the
    // pipeline does not carry is left for CompiledDimension::compile to
    // refuse, in its own words.
    const auto settingsEntry = pipeline.settings.find(settings);
    if (settingsEntry != pipeline.settings.end()) {
        const settings::NoiseGeometry& geometry = settingsEntry->second.geometry;
        if (geometry.minY != kOverworldMinY || geometry.height != kOverworldHeight) {
            throw NukkitError(
                "stratum-nukkit: noise settings '" + settings.toString() + "' span y " +
                std::to_string(geometry.minY) + ".." + std::to_string(geometry.maxY() - 1) +
                ", and the only Nukkit dimension this binding fills is the overworld, y " +
                std::to_string(kOverworldMinY) + ".." +
                std::to_string(kOverworldMinY + kOverworldHeight - 1));
        }
        impl.emittableStates = emittableBlockStates(settingsEntry->second);
    }
    const auto& list = biomes.parameterList();
    if (list.has_value()) {
        const auto listEntry = pipeline.biomeParameters.find(*list);
        if (listEntry != pipeline.biomeParameters.end()) {
            std::set<data::ResourceLocation> seen;
            std::vector<std::string> unmapped;
            for (const biome::Entry& entry : listEntry->second.entries()) {
                if (!seen.insert(entry.biome).second) {
                    continue;
                }
                const std::optional<std::int32_t> bedrockId = mapping::bedrockBiomeId(entry.biome);
                if (!bedrockId.has_value()) {
                    unmapped.push_back(entry.biome.toString());
                    continue;
                }
                impl.emittableBiomes.push_back(Biome{.java = entry.biome, .bedrockId = *bedrockId});
            }
            if (!unmapped.empty()) {
                std::string names;
                for (const std::string& name : unmapped) {
                    names += (names.empty() ? "'" : ", '") + name + "'";
                }
                throw NukkitError("stratum-nukkit: the biome source '" + biomes.toString() +
                                  "' can return " + names +
                                  ", which this build's Bedrock biome table has no id for; a "
                                  "custom biome has no Bedrock fallback in v1, so this world "
                                  "is refused rather than generated with a guessed biome");
            }
        }
    }

    impl.dimension =
        world::CompiledDimension::compile(std::move(pipeline), settings, biomes, worldSeed);

    // Dedupe by state id as well: `minecraft:water` and `minecraft:water[
    // level=0]` are spelt differently and are one state.
    std::vector<settings::BlockState> distinct;
    for (settings::BlockState& state : impl.emittableStates) {
        const std::uint32_t id = mapping::javaBlockStateId(state);
        if (std::find(impl.emittableStateIds.begin(), impl.emittableStateIds.end(), id) ==
            impl.emittableStateIds.end()) {
            impl.emittableStateIds.push_back(id);
            distinct.push_back(std::move(state));
        }
    }
    impl.emittableStates = std::move(distinct);
    return result;
}

std::unique_ptr<Pipeline> Pipeline::open(const std::filesystem::path& blobPath,
                                         const data::ResourceLocation& settings,
                                         const world::BiomeSource& biomes,
                                         const std::int64_t worldSeed) {
    const std::vector<std::byte> blob = readFile(blobPath);
    return compile(freeze::read(blob), settings, biomes, worldSeed);
}

std::int32_t Pipeline::minY() const noexcept {
    return impl_->dimension->geometry().minY;
}

std::int32_t Pipeline::height() const noexcept {
    return impl_->dimension->geometry().height;
}

const std::vector<settings::BlockState>& Pipeline::emittableStates() const noexcept {
    return impl_->emittableStates;
}

const std::vector<std::uint32_t>& Pipeline::emittableStateIds() const noexcept {
    return impl_->emittableStateIds;
}

const std::vector<Pipeline::Biome>& Pipeline::emittableBiomes() const noexcept {
    return impl_->emittableBiomes;
}

void Pipeline::requireComplete(const BlockIdTable& table) const {
    std::string missing;
    std::size_t count = 0;
    for (std::size_t i = 0; i < impl_->emittableStateIds.size(); ++i) {
        if (!table.find(impl_->emittableStateIds[i]).has_value()) {
            missing += (missing.empty() ? "" : ", ") + describe(impl_->emittableStates[i]);
            ++count;
        }
    }
    if (count != 0) {
        throw NukkitError("stratum-nukkit: " + std::to_string(count) + " of the " +
                          std::to_string(impl_->emittableStateIds.size()) +
                          " block states this world can place have no Nukkit id: " + missing);
    }
}

void Pipeline::fill(const std::int32_t chunkX, const std::int32_t chunkZ, const BlockIdTable& table,
                    const std::span<std::int32_t> fullBlockIds,
                    const std::span<std::int32_t> biomeIds,
                    std::vector<std::int32_t>* const fluidUpdates) const {
    const settings::NoiseGeometry& geometry = impl_->dimension->geometry();

    // Checked before anything expensive runs: a caller that sized its arrays
    // wrong should not first pay for a chunk it cannot be handed.
    if (fullBlockIds.size() != static_cast<std::size_t>(kChunkWidth) * kChunkWidth *
                                   static_cast<std::size_t>(geometry.height)) {
        throw NukkitError("stratum-nukkit: fullBlockIds holds " +
                          std::to_string(fullBlockIds.size()) + " entries, and this dimension's " +
                          std::to_string(geometry.height) + " blocks of height need " +
                          std::to_string(kChunkWidth * kChunkWidth * geometry.height) +
                          " — the caller and the pipeline disagree on geometry");
    }
    const std::int32_t quartsPerColumn = javamath::floorDiv(geometry.height, kQuartWidth);
    const std::size_t quartCount = static_cast<std::size_t>(quartsPerColumn) * kQuartsPerChunkEdge *
                                   static_cast<std::size_t>(kQuartsPerChunkEdge);
    if (biomeIds.size() != quartCount) {
        throw NukkitError("stratum-nukkit: biomeIds holds " + std::to_string(biomeIds.size()) +
                          " entries, and this dimension's quarts need " +
                          std::to_string(quartCount) +
                          " — the caller and the pipeline disagree on geometry");
    }

    terrain::ChunkBuffer buffer(geometry);
    impl_->dimension->fillBlocks(chunkX, chunkZ, buffer);

    // Each distinct state once per chunk, never once per block.
    const std::vector<settings::BlockState>& palette = buffer.palette();
    std::vector<std::int32_t> paletteIds(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const std::optional<std::int32_t> fullId =
            table.find(mapping::javaBlockStateId(palette[i]));
        if (!fullId.has_value()) {
            throw NukkitError("stratum-nukkit: chunk " + std::to_string(chunkX) + ", " +
                              std::to_string(chunkZ) + " places '" + describe(palette[i]) +
                              "', which this world's block id table does not resolve — it was "
                              "not among the states the world was opened for, so the list of "
                              "states lib/ can place has fallen behind it");
        }
        paletteIds[i] = *fullId;
    }
    for (std::int32_t ly = 0; ly < geometry.height; ++ly) {
        const std::int32_t y = geometry.minY + ly;
        for (int lz = 0; lz < kChunkWidth; ++lz) {
            for (int lx = 0; lx < kChunkWidth; ++lx) {
                const std::size_t index =
                    (static_cast<std::size_t>(ly) * kChunkWidth + static_cast<std::size_t>(lz)) *
                        kChunkWidth +
                    static_cast<std::size_t>(lx);
                fullBlockIds[index] = paletteIds[buffer.paletteIndexAt(lx, y, lz)];
            }
        }
    }

    std::vector<const data::ResourceLocation*> javaBiomes(quartCount);
    impl_->dimension->fillBiomes(chunkX, chunkZ, javaBiomes);
    for (std::size_t index = 0; index < javaBiomes.size(); ++index) {
        const std::optional<std::int32_t> bedrockId = mapping::bedrockBiomeId(*javaBiomes[index]);
        if (!bedrockId.has_value()) {
            // Unreachable for a multi-noise source compile() let through,
            // which checked every biome its list can return.
            throw NukkitError("stratum-nukkit: biome '" + javaBiomes[index]->toString() +
                              "' has no Bedrock id in this build's table");
        }
        biomeIds[index] = *bedrockId;
    }

    if (fluidUpdates != nullptr) {
        fluidUpdates->clear();
        fluidUpdates->reserve(buffer.fluidUpdates().size());
        for (const terrain::ChunkBuffer::FluidUpdate& update : buffer.fluidUpdates()) {
            // y - minY is in [0, height), so every field is non-negative and
            // the packed value fits an int32 for any height below 2^23.
            const std::uint32_t packed =
                static_cast<std::uint32_t>(update.localX) |
                (static_cast<std::uint32_t>(update.localZ) << 4U) |
                (static_cast<std::uint32_t>(update.y - geometry.minY) << 8U);
            fluidUpdates->push_back(static_cast<std::int32_t>(packed));
        }
    }
}

} // namespace stratum::nukkit
