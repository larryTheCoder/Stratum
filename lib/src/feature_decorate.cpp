// Stratum — the decoration stage: the order features run in, the placement
// modifiers, and the features themselves.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Every rule here is measured against the server and says by which probe
// (tools/analysis/feature-probe.sh; SPEC §11, "The decoration framework,
// measured"). feature/feature.hpp states the rules; this file runs them.

#include "feature_internal.hpp"

#include <stratum/carver/trig.hpp>
#include <stratum/javamath.hpp>
#include <stratum/math/fdlibm.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>

namespace stratum::feature {
namespace {

using detail::BlockClass;
using detail::classOf;

constexpr std::int32_t kChunkWidth = 16;
constexpr std::int32_t kQuartsPerChunk = 4;
constexpr std::size_t kColumnsAcross = 16;
constexpr std::size_t kQuartsAcross = 4;

/// The entry a weighted draw of @p weights picks.
[[nodiscard]] std::size_t weightedIndex(const std::vector<std::int32_t>& weights,
                                        rng::WorldgenRandom& random) {
    std::int32_t total = 0;
    for (const std::int32_t weight : weights) {
        total += weight;
    }
    std::int32_t draw = random.nextInt(total);
    for (std::size_t index = 0; index < weights.size(); ++index) {
        if (draw < weights[index]) {
            return index;
        }
        draw -= weights[index];
    }
    return weights.size() - 1;
}

} // namespace

std::int32_t IntProvider::sample(rng::WorldgenRandom& random) const {
    switch (kind) {
        case Kind::Constant:
            return min;
        case Kind::Uniform:
            return random.nextIntBetween(min, max);
        case Kind::BiasedToBottom:
            return min + random.nextInt(random.nextInt(max - min + 1) + 1);
        case Kind::Clamped:
            return std::clamp(children.front().sample(random), min, max);
        case Kind::ClampedNormal: {
            // The product's width is not measured to the last bit: single
            // precision, as the provider's two fields are.
            const float value = mean + (static_cast<float>(random.nextGaussian()) * deviation);
            const float clamped =
                std::clamp(value, static_cast<float>(min), static_cast<float>(max));
            return static_cast<std::int32_t>(clamped);
        }
        case Kind::WeightedList:
            return children[weightedIndex(weights, random)].sample(random);
    }
    return min;
}

std::int32_t IntProvider::lowest() const {
    if (kind == Kind::WeightedList) {
        std::int32_t least = INT32_MAX;
        for (const IntProvider& child : children) {
            least = std::min(least, child.lowest());
        }
        return least;
    }
    return min;
}

std::int32_t IntProvider::highest() const {
    if (kind == Kind::WeightedList) {
        std::int32_t most = INT32_MIN;
        for (const IntProvider& child : children) {
            most = std::max(most, child.highest());
        }
        return most;
    }
    return max;
}

std::int32_t HeightProvider::sample(rng::WorldgenRandom& random,
                                    const settings::NoiseGeometry& geometry) const {
    if (kind == Kind::WeightedList) {
        return children[weightedIndex(weights, random)].sample(random, geometry);
    }
    const std::int32_t low = min.resolve(geometry);
    const std::int32_t high = max.resolve(geometry);
    const std::int64_t range = std::int64_t{high} - low;
    if (kind == Kind::Constant || range <= 0) {
        return low;
    }
    const auto span = static_cast<std::int32_t>(std::min<std::int64_t>(range, INT32_MAX - 1));
    switch (kind) {
        case Kind::Uniform:
            return low + random.nextInt(span + 1);
        case Kind::BiasedToBottom: {
            if (span - inner + 1 <= 0) {
                return low;
            }
            return low + random.nextInt(random.nextInt(span - inner + 1) + inner);
        }
        case Kind::VeryBiasedToBottom: {
            if (span - inner + 1 <= 0) {
                return low;
            }
            const std::int32_t first = random.nextInt(span - inner + 1) + inner;
            const std::int32_t second = random.nextInt(first) + inner;
            return low + random.nextInt(second);
        }
        case Kind::Trapezoid: {
            if (plateau >= span) {
                return low + random.nextInt(span + 1);
            }
            const std::int32_t slope = (span - plateau) / 2;
            const std::int32_t rest = span - slope;
            const std::int32_t first = random.nextInt(rest + 1);
            return low + first + random.nextInt(slope + 1);
        }
        case Kind::Constant:
        case Kind::WeightedList:
            break;
    }
    return low;
}

const settings::BlockState& StateProvider::sample(rng::WorldgenRandom& random) const {
    if (kind == Kind::Simple) {
        return states.front();
    }
    return states[weightedIndex(weights, random)];
}

FeatureSources gatherFeatureSources(const data::Pack& pack, const data::BlockTags& tags) {
    FeatureSources sources;
    for (const auto& [tag, entries] : tags.entries()) {
        static_cast<void>(entries);
        sources.tags.emplace(tag, tags.resolve(tag));
    }
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::ConfiguredFeature)) {
        sources.configured.emplace(entry->id, entry->json);
    }
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::PlacedFeature)) {
        sources.placed.emplace(entry->id, entry->json);
    }
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::Biome)) {
        if (!entry->json.contains("features")) {
            throw FeatureError("biome " + entry->id.toString() + " has no features field");
        }
        sources.biomeFeatures.emplace(entry->id, entry->json.at("features"));
    }
    return sources;
}

namespace {

/// One step's graph, and the walk that orders it (FeatureSet::build).
class StepOrder {
public:
    /// The node of @p id, added at the end if new.
    std::size_t node(const data::ResourceLocation& id) {
        const auto [found, inserted] = index_.try_emplace(id, ids_.size());
        if (inserted) {
            ids_.push_back(id);
            successors_.emplace_back();
        }
        return found->second;
    }

    void edge(const std::size_t from, const std::size_t to) { successors_[from].insert(to); }

    /// The ids in the order they run. Throws FeatureError naming a feature on
    /// a cycle.
    [[nodiscard]] std::vector<data::ResourceLocation> ordered(const std::size_t step) const {
        enum class Mark : std::uint8_t { Unseen, Open, Done };
        std::vector<Mark> marks(ids_.size(), Mark::Unseen);
        std::vector<std::size_t> postOrder;
        postOrder.reserve(ids_.size());

        // An explicit stack: a pack's list may be long, and the walk's depth
        // is its length.
        struct Frame {
            std::size_t node;
            std::set<std::size_t>::const_iterator next;
        };

        std::vector<Frame> stack;
        for (std::size_t root = 0; root < ids_.size(); ++root) {
            if (marks[root] != Mark::Unseen) {
                continue;
            }
            marks[root] = Mark::Open;
            stack.push_back({root, successors_[root].begin()});
            while (!stack.empty()) {
                Frame& frame = stack.back();
                if (frame.next == successors_[frame.node].end()) {
                    marks[frame.node] = Mark::Done;
                    postOrder.push_back(frame.node);
                    stack.pop_back();
                    continue;
                }
                const std::size_t successor = *frame.next;
                ++frame.next;
                if (marks[successor] == Mark::Open) {
                    throw FeatureError("generation step " + std::to_string(step) +
                                       " has no feature order: the biomes' lists place " +
                                       ids_[successor].toString() + " both before and after " +
                                       ids_[frame.node].toString());
                }
                if (marks[successor] == Mark::Unseen) {
                    marks[successor] = Mark::Open;
                    stack.push_back({successor, successors_[successor].begin()});
                }
            }
        }
        std::vector<data::ResourceLocation> result;
        result.reserve(ids_.size());
        for (const std::size_t node : std::ranges::reverse_view(postOrder)) {
            result.push_back(ids_[node]);
        }
        return result;
    }

private:
    std::map<data::ResourceLocation, std::size_t> index_;
    std::vector<data::ResourceLocation> ids_;
    /// Ordered by node, which is first-seen order.
    std::vector<std::set<std::size_t>> successors_;
};

/// A biome's `features` field, as lists of ids.
[[nodiscard]] std::vector<std::vector<data::ResourceLocation>>
biomeLists(const nlohmann::json& field, const data::ResourceLocation& biome) {
    if (!field.is_array()) {
        throw FeatureError("biome " + biome.toString() + ": features must be a list of steps");
    }
    std::vector<std::vector<data::ResourceLocation>> lists;
    for (const nlohmann::json& step : field) {
        if (!step.is_array()) {
            throw FeatureError("biome " + biome.toString() + ": step " +
                               std::to_string(lists.size()) +
                               " of its features is not a list of placed feature ids (a tag or "
                               "a single id there is not implemented)");
        }
        std::vector<data::ResourceLocation>& list = lists.emplace_back();
        for (const nlohmann::json& entry : step) {
            const std::optional<data::ResourceLocation> id =
                entry.is_string() ? data::ResourceLocation::tryParse(entry.get<std::string>())
                                  : std::nullopt;
            if (!id.has_value()) {
                throw FeatureError("biome " + biome.toString() + ": step " +
                                   std::to_string(lists.size() - 1) +
                                   " of its features holds an entry that is not a placed feature "
                                   "id (one defined inline is not implemented)");
            }
            if (std::ranges::find(list, *id) != list.end()) {
                throw FeatureError("biome " + biome.toString() + " lists " + id->toString() +
                                   " twice in step " + std::to_string(lists.size() - 1));
            }
            list.push_back(*id);
        }
    }
    return lists;
}

} // namespace

FeatureSet FeatureSet::build(const FeatureSources& sources,
                             const std::span<const data::ResourceLocation> biomes) {
    std::vector<std::vector<std::vector<data::ResourceLocation>>> lists;
    lists.reserve(biomes.size());
    std::size_t stepCount = 0;
    for (const data::ResourceLocation& biome : biomes) {
        const auto found = sources.biomeFeatures.find(biome);
        if (found == sources.biomeFeatures.end()) {
            throw FeatureError("biome " + biome.toString() +
                               " is one the dimension generates and the pack does not define");
        }
        lists.push_back(biomeLists(found->second, biome));
        stepCount = std::max(stepCount, lists.back().size());
    }

    FeatureSet set;
    set.steps_.resize(stepCount);
    std::map<data::ResourceLocation, std::shared_ptr<const PlacedFeature>> compiled;
    for (std::size_t step = 0; step < stepCount; ++step) {
        StepOrder order;
        for (const auto& biomeSteps : lists) {
            if (step >= biomeSteps.size()) {
                continue;
            }
            std::optional<std::size_t> previous;
            for (const data::ResourceLocation& id : biomeSteps[step]) {
                const std::size_t node = order.node(id);
                if (previous.has_value()) {
                    order.edge(*previous, node);
                }
                previous = node;
            }
        }
        for (data::ResourceLocation& id : order.ordered(step)) {
            auto known = compiled.find(id);
            if (known == compiled.end()) {
                const auto json = sources.placed.find(id);
                if (json == sources.placed.end()) {
                    throw FeatureError("a biome lists placed feature " + id.toString() +
                                       ", which the pack does not define");
                }
                std::shared_ptr<const PlacedFeature> feature;
                try {
                    feature = std::make_shared<const PlacedFeature>(
                        detail::compilePlaced(json->second, id.toString(), sources));
                    ++set.supported_;
                } catch (const Unsupported& reason) {
                    set.unsupported_.push_back({.id = id, .reason = reason.what()});
                }
                known = compiled.emplace(id, std::move(feature)).first;
            }
            set.steps_[step].push_back({.id = std::move(id), .feature = known->second});
        }
    }

    for (std::size_t biome = 0; biome < biomes.size(); ++biome) {
        std::vector<std::vector<bool>>& listed = set.listed_[biomes[biome]];
        listed.resize(stepCount);
        for (std::size_t step = 0; step < stepCount; ++step) {
            const std::vector<Entry>& entries = set.steps_[step];
            listed[step].assign(entries.size(), false);
            if (step >= lists[biome].size()) {
                continue;
            }
            for (std::size_t index = 0; index < entries.size(); ++index) {
                listed[step][index] = std::ranges::find(lists[biome][step], entries[index].id) !=
                                      lists[biome][step].end();
            }
        }
    }
    return set;
}

bool FeatureSet::lists(const data::ResourceLocation& biome, const std::size_t step,
                       const std::size_t index) const noexcept {
    const auto found = listed_.find(biome);
    return found != listed_.end() && step < found->second.size() &&
           index < found->second[step].size() && found->second[step][index];
}

std::vector<settings::BlockState> FeatureSet::writtenStates() const {
    std::vector<settings::BlockState> states;
    for (const std::vector<Entry>& step : steps_) {
        for (const Entry& entry : step) {
            if (entry.feature == nullptr) {
                continue;
            }
            const auto add = [&](const settings::BlockState& state) {
                if (std::ranges::find(states, state) == states.end()) {
                    states.push_back(state);
                }
            };
            for (const settings::BlockState& state : entry.feature->feature->toPlace.states) {
                add(state);
            }
            for (const OreTarget& target : entry.feature->feature->targets) {
                add(target.state);
            }
            if (entry.feature->feature->type == ConfiguredFeature::Type::Spring) {
                add(entry.feature->feature->fluid);
            }
            for (const ConfiguredFeature::StateRule& rule : entry.feature->feature->stateRules) {
                for (const settings::BlockState& state : rule.then.states) {
                    add(state);
                }
            }
        }
    }
    return states;
}

TerrainHeightmaps terrainHeightmaps(const terrain::ChunkBuffer& blocks) {
    TerrainHeightmaps maps;
    const std::int32_t minY = blocks.minY();
    const std::int32_t top = minY + blocks.height();
    for (int z = 0; z < kChunkWidth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const std::size_t column =
                (static_cast<std::size_t>(z) * kColumnsAcross) + static_cast<std::size_t>(x);
            maps.worldSurface.at(column) = minY;
            maps.oceanFloor.at(column) = minY;
            bool surfaceKnown = false;
            for (std::int32_t y = top - 1; y >= minY; --y) {
                const BlockClass kind = classOf(blocks.at(x, y, z));
                if (kind == BlockClass::Air) {
                    continue;
                }
                if (!surfaceKnown) {
                    maps.worldSurface.at(column) = y + 1;
                    surfaceKnown = true;
                }
                if (kind == BlockClass::Blocking) {
                    maps.oceanFloor.at(column) = y + 1;
                    break;
                }
            }
        }
    }
    return maps;
}

namespace {

struct Position {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

/// The region as one world: blocks, heightmaps and biomes by block position.
class World {
public:
    explicit World(Region& region)
        : region_(&region), zoom_(region.worldSeed), minY_(region.geometry.minY),
          maxY_(region.geometry.maxY()) {}

    [[nodiscard]] std::int32_t minY() const noexcept { return minY_; }

    [[nodiscard]] std::int32_t maxY() const noexcept { return maxY_; }

    [[nodiscard]] bool insideY(const std::int32_t y) const noexcept {
        return y >= minY_ && y < maxY_;
    }

    /// The block at a position; air outside the dimension's height and
    /// outside the nine chunks, where nothing is known.
    [[nodiscard]] const settings::BlockState& block(const Position at) const {
        static const settings::BlockState kAir;
        const RegionChunk* chunk = chunkOf(at.x, at.z);
        if (chunk == nullptr || !insideY(at.y)) {
            return kAir;
        }
        return chunk->blocks->at(javamath::floorMod(at.x, kChunkWidth), at.y,
                                 javamath::floorMod(at.z, kChunkWidth));
    }

    /// Writes @p state; a position outside the dimension or the nine chunks
    /// is not written.
    void set(const Position at, const settings::BlockState& state) {
        const RegionChunk* chunk = chunkOf(at.x, at.z);
        if (chunk == nullptr || !insideY(at.y)) {
            return;
        }
        chunk->blocks->set(javamath::floorMod(at.x, kChunkWidth), at.y,
                           javamath::floorMod(at.z, kChunkWidth), state);
    }

    /// Marks a position for a fluid update, once, where it can be written.
    void markFluidUpdate(const Position at) {
        const RegionChunk* chunk = chunkOf(at.x, at.z);
        if (chunk == nullptr || !insideY(at.y)) {
            return;
        }
        const terrain::ChunkBuffer::FluidUpdate mark{
            .localX = static_cast<std::uint8_t>(javamath::floorMod(at.x, kChunkWidth)),
            .y = at.y,
            .localZ = static_cast<std::uint8_t>(javamath::floorMod(at.z, kChunkWidth))};
        const auto& marks = chunk->blocks->fluidUpdates();
        if (std::ranges::find(marks, mark) == marks.end()) {
            chunk->blocks->markFluidUpdate(mark.localX, mark.y, mark.localZ);
        }
    }

    /// Whether the column is one of the nine chunks'.
    [[nodiscard]] bool loaded(const std::int32_t x, const std::int32_t z) const noexcept {
        return chunkOf(x, z) != nullptr;
    }

    /// The y above the column's highest block of the kind's class; the
    /// dimension's minimum y for a column with none, or outside the region.
    [[nodiscard]] std::int32_t height(const HeightmapKind kind, const std::int32_t x,
                                      const std::int32_t z) const {
        const RegionChunk* chunk = chunkOf(x, z);
        if (chunk == nullptr) {
            return minY_;
        }
        const int localX = javamath::floorMod(x, kChunkWidth);
        const int localZ = javamath::floorMod(z, kChunkWidth);
        const std::size_t column =
            (static_cast<std::size_t>(localZ) * kColumnsAcross) + static_cast<std::size_t>(localX);
        if (kind == HeightmapKind::WorldSurfaceWg) {
            return chunk->heightmaps->worldSurface.at(column);
        }
        if (kind == HeightmapKind::OceanFloorWg) {
            return chunk->heightmaps->oceanFloor.at(column);
        }
        for (std::int32_t y = maxY_ - 1; y >= minY_; --y) {
            const settings::BlockState& state = chunk->blocks->at(localX, y, localZ);
            const BlockClass blockClass = classOf(state);
            bool counts = false;
            switch (kind) {
                case HeightmapKind::WorldSurface:
                    counts = blockClass != BlockClass::Air;
                    break;
                case HeightmapKind::OceanFloor:
                    counts = blockClass == BlockClass::Blocking;
                    break;
                case HeightmapKind::MotionBlocking:
                    counts = blockClass != BlockClass::Air && blockClass != BlockClass::Passable;
                    break;
                case HeightmapKind::MotionBlockingNoLeaves:
                    counts = blockClass != BlockClass::Air && blockClass != BlockClass::Passable &&
                             !detail::isLeaves(state);
                    break;
                case HeightmapKind::WorldSurfaceWg:
                case HeightmapKind::OceanFloorWg:
                    break;
            }
            if (counts) {
                return y + 1;
            }
        }
        return minY_;
    }

    /// The block-level biome: the quart the zoom reads the position from.
    [[nodiscard]] const data::ResourceLocation& biome(const Position at) const {
        const biome::QuartPos quart = zoom_.zoom(at.x, at.y, at.z);
        const std::int32_t chunkX = javamath::floorDiv(quart.x, kQuartsPerChunk);
        const std::int32_t chunkZ = javamath::floorDiv(quart.z, kQuartsPerChunk);
        const RegionChunk* chunk = chunkAt(chunkX, chunkZ);
        if (chunk == nullptr || chunk->biomes.empty()) {
            return region_->biomeOutside(quart);
        }
        const auto layers = static_cast<std::int32_t>(chunk->biomes.size() / 16);
        const std::int32_t layer = std::clamp(quart.y - chunk->minQuartY, 0, layers - 1);
        const std::size_t index =
            (((static_cast<std::size_t>(layer) * kQuartsAcross) +
              static_cast<std::size_t>(javamath::floorMod(quart.z, kQuartsPerChunk))) *
             kQuartsAcross) +
            static_cast<std::size_t>(javamath::floorMod(quart.x, kQuartsPerChunk));
        return *chunk->biomes[index];
    }

private:
    [[nodiscard]] const RegionChunk* chunkAt(const std::int32_t chunkX,
                                             const std::int32_t chunkZ) const noexcept {
        const std::int64_t dx = std::int64_t{chunkX} - region_->centreX;
        const std::int64_t dz = std::int64_t{chunkZ} - region_->centreZ;
        if (dx < -1 || dx > 1 || dz < -1 || dz > 1) {
            return nullptr;
        }
        const RegionChunk& chunk =
            region_->chunks.at(static_cast<std::size_t>(((dz + 1) * 3) + dx + 1));
        return chunk.blocks == nullptr ? nullptr : &chunk;
    }

    [[nodiscard]] const RegionChunk* chunkOf(const std::int32_t x,
                                             const std::int32_t z) const noexcept {
        return chunkAt(javamath::floorDiv(x, kChunkWidth), javamath::floorDiv(z, kChunkWidth));
    }

    Region* region_;
    biome::VoronoiZoom zoom_;
    std::int32_t minY_;
    std::int32_t maxY_;
};

[[nodiscard]] bool holds(const BlockPredicate& predicate, const World& world, const Position at) {
    const Position target{.x = at.x + predicate.offset[0],
                          .y = at.y + predicate.offset[1],
                          .z = at.z + predicate.offset[2]};
    switch (predicate.kind) {
        case BlockPredicate::Kind::True:
            return true;
        case BlockPredicate::Kind::MatchingBlocks:
            return predicate.blocks.contains(world.block(target).name);
        case BlockPredicate::Kind::MatchingFluids: {
            const BlockClass kind = classOf(world.block(target));
            return (predicate.water && kind == BlockClass::Water) ||
                   (predicate.lava && kind == BlockClass::Lava);
        }
        case BlockPredicate::Kind::InsideWorldBounds:
            return world.insideY(target.y);
        case BlockPredicate::Kind::AllOf:
            return std::ranges::all_of(predicate.children, [&](const BlockPredicate& child) {
                return holds(child, world, at);
            });
        case BlockPredicate::Kind::AnyOf:
            return std::ranges::any_of(predicate.children, [&](const BlockPredicate& child) {
                return holds(child, world, at);
            });
        case BlockPredicate::Kind::Not:
            return !holds(predicate.children.front(), world, at);
    }
    return false;
}

/// The noise the two noise counts read (feature.hpp, Modifier): the same in
/// every world, so built once.
[[nodiscard]] const noise::PerlinNoise& countNoise() {
    static const noise::PerlinNoise kNoise = [] {
        rng::JavaRandom random{2345};
        return noise::PerlinNoise::fromRandom(random);
    }();
    return kNoise;
}

/// One placed feature's run in one chunk: the modifiers depth first, so that
/// each position's feature draws before the next position is drawn
/// (MEASURED, --variant draws, plane 8: position, state, position, state).
class Placement {
public:
    Placement(const FeatureSet& features, const PlacedFeature& placed, const std::size_t step,
              const std::size_t index, World& world, const Region& region,
              rng::WorldgenRandom& random)
        : features_(&features), placed_(&placed), step_(step), index_(index), world_(&world),
          region_(&region), random_(&random) {}

    void run(const std::size_t modifier, Position at) {
        if (modifier == placed_->placement.size()) {
            place(at);
            return;
        }
        const Modifier& current = placed_->placement[modifier];
        switch (current.kind) {
            case Modifier::Kind::InSquare:
                at.x += random_->nextInt(kChunkWidth);
                at.z += random_->nextInt(kChunkWidth);
                break;
            case Modifier::Kind::Count: {
                const std::int32_t count = current.count.sample(*random_);
                for (std::int32_t copy = 0; copy < count; ++copy) {
                    run(modifier + 1, at);
                }
                return;
            }
            case Modifier::Kind::HeightRange:
                at.y = current.height.sample(*random_, region_->geometry);
                break;
            case Modifier::Kind::Heightmap: {
                const std::int32_t height = world_->height(current.heightmap, at.x, at.z);
                // NOT MEASURED: an empty column. The documented reading — nothing
                // is placed there.
                if (height <= world_->minY()) {
                    return;
                }
                at.y = height;
                break;
            }
            case Modifier::Kind::Biome:
                if (!features_->lists(world_->biome(at), step_, index_)) {
                    return;
                }
                break;
            case Modifier::Kind::BlockPredicateFilter:
                if (!holds(current.predicate, *world_, at)) {
                    return;
                }
                break;
            case Modifier::Kind::RarityFilter:
                if (!(random_->nextFloat() < 1.0F / static_cast<float>(current.chance))) {
                    return;
                }
                break;
            case Modifier::Kind::SurfaceWaterDepthFilter: {
                const std::int32_t floor = world_->height(HeightmapKind::OceanFloor, at.x, at.z);
                const std::int32_t surface =
                    world_->height(HeightmapKind::WorldSurface, at.x, at.z);
                if (surface - floor > current.maxWaterDepth) {
                    return;
                }
                break;
            }
            case Modifier::Kind::RandomOffset: {
                const std::int32_t dx = current.xzSpread.sample(*random_);
                const std::int32_t dy = current.ySpread.sample(*random_);
                const std::int32_t dz = current.xzSpread.sample(*random_);
                at.x += dx;
                at.y += dy;
                at.z += dz;
                break;
            }
            case Modifier::Kind::EnvironmentScan: {
                // MEASURED (--variant terrain, three scans): the start must be
                // allowed where a condition is named; the start itself may be the
                // target; then up to max_steps moves, each ending the walk at a
                // target or at a block the walk may not pass.
                if (current.hasAllowed && !holds(current.allowed, *world_, at)) {
                    return;
                }
                bool found = holds(current.predicate, *world_, at);
                for (std::int32_t move = 0; !found && move < current.maxSteps; ++move) {
                    at.y += current.scanUp ? 1 : -1;
                    // NOT MEASURED: a walk that leaves the dimension ends.
                    if (!world_->insideY(at.y)) {
                        return;
                    }
                    if (holds(current.predicate, *world_, at)) {
                        found = true;
                    } else if (current.hasAllowed && !holds(current.allowed, *world_, at)) {
                        return;
                    }
                }
                if (!found) {
                    return;
                }
                break;
            }
            case Modifier::Kind::SurfaceRelativeThresholdFilter: {
                const std::int64_t height = world_->height(current.heightmap, at.x, at.z);
                if (at.y < height + current.minInclusive || at.y > height + current.maxInclusive) {
                    return;
                }
                break;
            }
            case Modifier::Kind::NoiseThresholdCount: {
                constexpr double kScale = 200.0;
                const double noise = countNoise().sampleSimplex2D(
                    static_cast<double>(at.x) / kScale, static_cast<double>(at.z) / kScale);
                const std::int32_t count =
                    noise > current.noiseLevel ? current.aboveNoise : current.belowNoise;
                for (std::int32_t copy = 0; copy < count; ++copy) {
                    run(modifier + 1, at);
                }
                return;
            }
            case Modifier::Kind::NoiseBasedCount: {
                const double noise =
                    countNoise().sampleSimplex2D(static_cast<double>(at.x) / current.noiseFactor,
                                                 static_cast<double>(at.z) / current.noiseFactor);
                const double count =
                    std::ceil((noise + current.noiseOffset) * current.noiseToCountRatio);
                for (std::int32_t copy = 0; static_cast<double>(copy) < count; ++copy) {
                    run(modifier + 1, at);
                }
                return;
            }
            case Modifier::Kind::FixedPlacement:
                for (const std::array<std::int32_t, 3>& position : current.positions) {
                    if (javamath::floorDiv(position[0], kChunkWidth) == region_->centreX &&
                        javamath::floorDiv(position[2], kChunkWidth) == region_->centreZ) {
                        run(modifier + 1,
                            Position{.x = position[0], .y = position[1], .z = position[2]});
                    }
                }
                return;
        }
        run(modifier + 1, at);
    }

private:
    void place(const Position at) {
        const ConfiguredFeature& feature = *placed_->feature;
        switch (feature.type) {
            case ConfiguredFeature::Type::SimpleBlock:
                world_->set(at, feature.toPlace.sample(*random_));
                break;
            case ConfiguredFeature::Type::Ore:
                placeOre(feature, at);
                break;
            case ConfiguredFeature::Type::Spring:
                placeSpring(feature, at);
                break;
            case ConfiguredFeature::Type::Disk:
                placeDisk(feature, at);
                break;
        }
    }

    /// feature.hpp, ConfiguredFeature::Type::Disk.
    void placeDisk(const ConfiguredFeature& disk, const Position at) {
        const std::int32_t radius = disk.radius.sample(*random_);
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            for (std::int32_t dz = -radius; dz <= radius; ++dz) {
                if ((dx * dx) + (dz * dz) > radius * radius) {
                    continue;
                }
                for (std::int32_t y = at.y + disk.halfHeight; y >= at.y - disk.halfHeight; --y) {
                    const Position cell{.x = at.x + dx, .y = y, .z = at.z + dz};
                    if (!world_->insideY(y) || !world_->loaded(cell.x, cell.z) ||
                        !holds(disk.target, *world_, cell)) {
                        continue;
                    }
                    const StateProvider* provider = &disk.toPlace;
                    for (const ConfiguredFeature::StateRule& rule : disk.stateRules) {
                        if (holds(rule.ifTrue, *world_, cell)) {
                            provider = &rule.then;
                            break;
                        }
                    }
                    world_->set(cell, provider->sample(*random_));
                }
            }
        }
    }

    /// feature.hpp, ConfiguredFeature::Type::Spring.
    void placeSpring(const ConfiguredFeature& spring, const Position at) {
        const auto valid = [&](const Position where) {
            return spring.validBlocks.contains(world_->block(where).name);
        };
        const auto air = [&](const Position where) {
            return world_->insideY(where.y) && world_->loaded(where.x, where.z) &&
                   classOf(world_->block(where)) == BlockClass::Air;
        };
        const Position above{.x = at.x, .y = at.y + 1, .z = at.z};
        const Position below{.x = at.x, .y = at.y - 1, .z = at.z};
        if (!world_->insideY(at.y) || !valid(above) ||
            (spring.requiresBlockBelow && !valid(below)) || !(air(at) || valid(at))) {
            return;
        }
        const std::array<Position, 5> around = {Position{.x = at.x - 1, .y = at.y, .z = at.z},
                                                Position{.x = at.x + 1, .y = at.y, .z = at.z},
                                                Position{.x = at.x, .y = at.y, .z = at.z - 1},
                                                Position{.x = at.x, .y = at.y, .z = at.z + 1},
                                                below};
        std::int32_t rocks = 0;
        std::int32_t holes = 0;
        for (const Position& beside : around) {
            rocks += valid(beside) ? 1 : 0;
            holes += air(beside) ? 1 : 0;
        }
        if (rocks == spring.rockCount && holes == spring.holeCount) {
            world_->set(at, spring.fluid);
            world_->markFluidUpdate(at);
        }
    }

    /// feature.hpp, ConfiguredFeature::Type::Ore, formula for formula.
    void placeOre(const ConfiguredFeature& ore, const Position at) {
        constexpr float kPi = 3.14159265358979323846F;
        const auto size = static_cast<float>(ore.size);
        const float angle = random_->nextFloat() * kPi;
        const float half = size / 8.0F;
        const float alongX = static_cast<float>(fdlibm::sin(static_cast<double>(angle))) * half;
        const float alongZ = static_cast<float>(fdlibm::cos(static_cast<double>(angle))) * half;
        const double fromX = static_cast<double>(at.x) + static_cast<double>(alongX);
        const double toX = static_cast<double>(at.x) - static_cast<double>(alongX);
        const double fromZ = static_cast<double>(at.z) + static_cast<double>(alongZ);
        const double toZ = static_cast<double>(at.z) - static_cast<double>(alongZ);
        const double fromY = at.y + random_->nextInt(3) - 2;
        const double toY = at.y + random_->nextInt(3) - 2;

        const auto reach = static_cast<std::int32_t>(std::ceil(half));
        const auto radius = static_cast<std::int32_t>(std::ceil((half + 1.0F) / 2.0F));
        const std::int32_t lowest = at.y - 2 - radius;
        bool underground = false;
        for (std::int32_t x = at.x - reach - radius; !underground && x <= at.x + reach + radius;
             ++x) {
            for (std::int32_t z = at.z - reach - radius; z <= at.z + reach + radius; ++z) {
                if (lowest <= world_->height(HeightmapKind::OceanFloorWg, x, z)) {
                    underground = true;
                    break;
                }
            }
        }
        if (!underground) {
            return;
        }

        struct Node {
            double x;
            double y;
            double z;
            double radius;
        };

        std::array<Node, 64> nodes{};
        const auto count = static_cast<std::size_t>(ore.size);
        for (std::size_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / size;
            const auto along = static_cast<double>(t);
            const double spread = random_->nextDouble() * static_cast<double>(ore.size) / 16.0;
            const float profile = carver::tableSin(static_cast<double>(kPi * t)) + 1.0F;
            nodes.at(i) = {.x = fromX + ((toX - fromX) * along),
                           .y = fromY + ((toY - fromY) * along),
                           .z = fromZ + ((toZ - fromZ) * along),
                           .radius = ((static_cast<double>(profile) * spread) + 1.0) / 2.0};
        }

        // A node wholly inside another takes no part: its blocks are the
        // other's, and take their draws in the other's turn.
        for (std::size_t i = 0; i + 1 < count; ++i) {
            for (std::size_t j = i + 1; j < count; ++j) {
                Node& first = nodes.at(i);
                Node& second = nodes.at(j);
                if (first.radius <= 0.0 || second.radius <= 0.0) {
                    continue;
                }
                const double dx = first.x - second.x;
                const double dy = first.y - second.y;
                const double dz = first.z - second.z;
                const double difference = first.radius - second.radius;
                if (difference * difference > (dx * dx) + (dy * dy) + (dz * dz)) {
                    (difference > 0.0 ? second : first).radius = -1.0;
                }
            }
        }

        // The blob's blocks, each once, in the order their draws are taken.
        // Every centre is within s/8 <= 8 blocks of the position across and
        // within 2 below it, and every radius at most s/16 + 1/2 <= 4.5.
        constexpr std::int32_t kAcross = 14;
        constexpr std::int32_t kBelow = 8;
        constexpr std::size_t kWide = (2 * 14) + 1;
        constexpr std::size_t kTall = 16;
        std::array<std::uint64_t, ((kWide * kTall * kWide) / 64) + 1> visited{};
        for (std::size_t i = 0; i < count; ++i) {
            const Node& node = nodes.at(i);
            if (node.radius < 0.0) {
                continue;
            }
            const auto low = [&](const double centre) {
                return static_cast<std::int32_t>(std::floor(centre - node.radius));
            };
            const auto high = [&](const double centre) {
                return static_cast<std::int32_t>(std::floor(centre + node.radius));
            };
            for (std::int32_t x = low(node.x); x <= high(node.x); ++x) {
                const double dx = (static_cast<double>(x) + 0.5 - node.x) / node.radius;
                for (std::int32_t y = low(node.y); y <= high(node.y); ++y) {
                    const double dy = (static_cast<double>(y) + 0.5 - node.y) / node.radius;
                    for (std::int32_t z = low(node.z); z <= high(node.z); ++z) {
                        const double dz = (static_cast<double>(z) + 0.5 - node.z) / node.radius;
                        if (!((dx * dx) + (dy * dy) + (dz * dz) < 1.0)) {
                            continue;
                        }
                        const std::size_t bit =
                            (((static_cast<std::size_t>(x - at.x + kAcross) * kTall) +
                              static_cast<std::size_t>(y - at.y + kBelow)) *
                             kWide) +
                            static_cast<std::size_t>(z - at.z + kAcross);
                        std::uint64_t& word = visited.at(bit / 64);
                        const std::uint64_t mask = std::uint64_t{1} << (bit % 64);
                        if ((word & mask) != 0) {
                            continue;
                        }
                        word |= mask;
                        offerOre(ore, Position{.x = x, .y = y, .z = z});
                    }
                }
            }
        }
    }

    /// One block of an ore's blob, offered to its targets in turn.
    void offerOre(const ConfiguredFeature& ore, const Position cell) {
        if (!world_->insideY(cell.y) || !world_->loaded(cell.x, cell.z)) {
            return;
        }
        const data::ResourceLocation& present = world_->block(cell).name;
        for (const OreTarget& target : ore.targets) {
            if (target.test.kind == RuleTest::Kind::Blocks &&
                !target.test.blocks.contains(present)) {
                continue;
            }
            if (ore.discardChance > 0.0F) {
                // A chance of 1 draws nothing; any other draws first.
                const bool drawn =
                    ore.discardChance >= 1.0F || random_->nextFloat() < ore.discardChance;
                if (drawn && besideAir(cell)) {
                    // NOT MEASURED: whether a discarded block goes on to the
                    // next target. minecraft.wiki's history for the ore
                    // feature says it did until a later version; so here.
                    continue;
                }
            }
            world_->set(cell, target.state);
            return;
        }
    }

    [[nodiscard]] bool besideAir(const Position cell) const {
        constexpr std::array<std::array<std::int32_t, 3>, 6> kSides = {
            {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        return std::ranges::any_of(kSides, [&](const std::array<std::int32_t, 3>& side) {
            const Position beside{
                .x = cell.x + side[0], .y = cell.y + side[1], .z = cell.z + side[2]};
            return world_->insideY(beside.y) && world_->loaded(beside.x, beside.z) &&
                   classOf(world_->block(beside)) == BlockClass::Air;
        });
    }

    const FeatureSet* features_;
    const PlacedFeature* placed_;
    std::size_t step_;
    std::size_t index_;
    World* world_;
    const Region* region_;
    rng::WorldgenRandom* random_;
};

} // namespace

void decorate(const FeatureSet& features, Region& region) {
    for (const RegionChunk& chunk : region.chunks) {
        if (chunk.blocks == nullptr || chunk.heightmaps == nullptr) {
            throw FeatureError("decorating chunk (" + std::to_string(region.centreX) + ", " +
                               std::to_string(region.centreZ) +
                               ") needs all nine chunks around it, filled");
        }
    }
    // The biomes of the nine chunks decide which features run.
    std::vector<const data::ResourceLocation*> present;
    for (const RegionChunk& chunk : region.chunks) {
        for (const data::ResourceLocation* biome : chunk.biomes) {
            // By name: a biome source may hold one biome at several
            // addresses (a multi-noise list's rows).
            if (std::ranges::none_of(present, [&](const data::ResourceLocation* known) {
                    return known == biome || *known == *biome;
                })) {
                present.push_back(biome);
            }
        }
    }

    World world(region);
    const std::int32_t originX = region.centreX * kChunkWidth;
    const std::int32_t originZ = region.centreZ * kChunkWidth;
    const std::int64_t population = rng::populationSeed(region.worldSeed, originX, originZ);
    const std::vector<std::vector<FeatureSet::Entry>>& steps = features.steps();
    for (std::size_t step = 0; step < steps.size(); ++step) {
        for (std::size_t index = 0; index < steps[step].size(); ++index) {
            const FeatureSet::Entry& entry = steps[step][index];
            if (entry.feature == nullptr ||
                std::ranges::none_of(present, [&](const data::ResourceLocation* biome) {
                    return features.lists(*biome, step, index);
                })) {
                continue;
            }
            rng::WorldgenRandom random(rng::featureSeed(
                population, static_cast<std::int32_t>(index), static_cast<std::int32_t>(step)));
            Placement placement(features, *entry.feature, step, index, world, region, random);
            placement.run(0, Position{.x = originX, .y = region.geometry.minY, .z = originZ});
        }
    }
}

} // namespace stratum::feature
