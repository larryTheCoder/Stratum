// Stratum — the multi-noise biome parameter table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ADAPTED (CLAUDE.md, NOTICE): the fixed-point mapping every sample and bound
// goes through here, quantizeCoord, is adapted from cubiomes
// (https://github.com/Cubitect/cubiomes, MIT License, Copyright (c) 2020
// Cubitect), biomenoise.c `sampleBiomeNoise`, read at
// e61f90580cbdd883214a8054670dacae655e59c0. parameter_list.hpp's header says
// what is adapted and what is this project's own.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace stratum::biome {

namespace {

/// A table larger than this is a corrupt file rather than a dimension.
/// Vanilla's overworld is 7593 rows.
constexpr std::size_t kMaxEntries = 1'000'000;

[[noreturn]] void fail(const data::ResourceLocation& id, const std::string& what) {
    throw ParameterError("biome parameter list '" + id.toString() + "': " + what);
}

/// `[min, max]`, or a bare number for a range of zero width. Both spellings
/// appear in what the server writes — `depth: 0.0` beside
/// `continentalness: [-1.2, -1.05]` — so both are read rather than one being
/// treated as the malformed version of the other.
[[nodiscard]] Parameter readParameter(const nlohmann::json& value, const data::ResourceLocation& id,
                                      const char* axis, std::size_t row) {
    const std::string where = std::string(axis) + " of entry " + std::to_string(row);
    if (value.is_number()) {
        const double point = value.get<double>();
        return Parameter{.min = point, .max = point};
    }
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
        fail(id, where + " must be a number or a pair of numbers");
    }
    const Parameter parameter{.min = value[0].get<double>(), .max = value[1].get<double>()};
    if (parameter.min > parameter.max) {
        fail(id, where + " has its minimum above its maximum");
    }
    return parameter;
}

/// `value` as a message prints it: every digit a double carries.
[[nodiscard]] std::string spell(double value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}

/// One climate reading of a sample, refused by name where the search is not
/// defined for it (kMaxClimateSample), NaN included.
void checkSample(double value, const char* axis) {
    if (std::isnan(value) || value < -kMaxClimateSample || value > kMaxClimateSample) {
        throw ParameterError(std::string("the biome search was handed a ") + axis + " of " +
                             spell(value) + ", outside ±" + spell(kMaxClimateSample) +
                             " (or not a number): past it the search's fixed-point arithmetic "
                             "overflows, and what the server picks there is unmeasured, so this "
                             "build refuses it (SPEC §8). Check that router entry");
    }
}

void checkSamples(const ClimateSample& sample) {
    checkSample(sample.temperature, "temperature");
    checkSample(sample.humidity, "humidity (vegetation)");
    checkSample(sample.continentalness, "continentalness (continents)");
    checkSample(sample.erosion, "erosion");
    checkSample(sample.depth, "depth");
    checkSample(sample.weirdness, "weirdness (ridges)");
}

/// The same refusal for a sample handed over already quantised.
void checkSamples(const QuantizedSample& sample) {
    for (const auto& [axis, value] :
         {std::pair{"temperature", sample.temperature}, std::pair{"humidity", sample.humidity},
          std::pair{"continentalness", sample.continentalness},
          std::pair{"erosion", sample.erosion}, std::pair{"depth", sample.depth},
          std::pair{"weirdness", sample.weirdness}}) {
        if (value < -kMaxQuantizedSample || value > kMaxQuantizedSample) {
            throw ParameterError(std::string("the biome search was handed a quantised ") + axis +
                                 " of " + std::to_string(value) + ", outside ±" +
                                 std::to_string(kMaxQuantizedSample) +
                                 ": past it the search's fixed-point arithmetic overflows "
                                 "(SPEC §8)");
        }
    }
}

void checkNotEmpty(const std::vector<Entry>& entries) {
    if (entries.empty()) {
        throw ParameterError("this dimension's biome parameter list is empty, so there is no "
                             "biome to choose; refusing rather than inventing one");
    }
}

[[nodiscard]] constexpr std::array<std::int64_t, 6> axesOf(const QuantizedSample& sample) noexcept {
    return {sample.temperature, sample.humidity, sample.continentalness,
            sample.erosion,     sample.depth,    sample.weirdness};
}

/// One axis's term of the fitness: the squared distance from @p value to
/// [@p min, @p max], zero inside. QuantizedPoint::fitness and the search
/// tree's boxes both score through this one function, so a node's bound and
/// a row's fitness are the same int64 arithmetic.
[[nodiscard]] constexpr std::int64_t axisTerm(std::int64_t value, std::int64_t min,
                                              std::int64_t max) noexcept {
    // How far the sample lies outside the range — zero when inside.
    std::int64_t distance = 0;
    if (value < min) {
        distance = min - value;
    } else if (value > max) {
        distance = value - max;
    }
    return distance * distance;
}

/// QuantizedPoint::fitness over a search box: the offset squared, then the
/// six axes' terms in axis order. For a row's box this IS that row's fitness
/// (the same integers, widened from int32); for a node's it is the node's
/// lower bound. Integer arithmetic inside int64 for every input the list
/// lets through (see QuantizedPoint::fitness), so it is exact.
[[nodiscard]] std::int64_t score(const detail::SearchBox& box,
                                 const std::array<std::int64_t, 6>& values) noexcept {
    // The six axes spelled out rather than looped over: the same terms,
    // added in the same order, but the loop is not unrolled for us and its
    // counter and its reloads of the sample cost as much as the terms.
    const auto term = [&](const std::size_t axis) noexcept {
        return axisTerm(values[axis], box.min[axis], box.max[axis]);
    };
    std::int64_t total = std::int64_t{box.offset} * std::int64_t{box.offset};
    total += term(0);
    total += term(1);
    total += term(2);
    total += term(3);
    total += term(4);
    total += term(5);
    return total;
}

/// Leaves hold at most this many rows. The analysts' prototype measured 8
/// as the balance between rows scored and node bounds computed per query
/// (levers-round1, item A): 4 scores fewer rows and more nodes, 16 the
/// reverse.
constexpr std::size_t kLeafRows = 8;

/// Up to this depth a node is split where it costs least; below it, at the
/// median. The cheapest split can be lopsided — for a list of identical rows
/// every split costs the same — and the median halves, so a tree is at most
/// this plus log2(kMaxEntries / kLeafRows) = 17 deep, inside kMaxSearchDepth.
constexpr std::size_t kCostSplitDepth = 40;

/// A box's margin: its extent summed over the six axes. The split cost is
/// each child's margin weighted by its row count — a surface-area heuristic
/// for six dimensions, and the prototype's.
[[nodiscard]] std::int64_t margin(const detail::SearchBox& box) noexcept {
    std::int64_t total = 0;
    for (std::size_t axis = 0; axis < 6; ++axis) {
        total += std::int64_t{box.max[axis]} - std::int64_t{box.min[axis]};
    }
    return total;
}

/// A box that holds nothing yet: enclose() of any row makes it that row's.
[[nodiscard]] detail::SearchBox emptyBox() noexcept {
    detail::SearchBox box;
    box.min.fill(std::numeric_limits<std::int32_t>::max());
    box.max.fill(std::numeric_limits<std::int32_t>::min());
    box.offset = std::numeric_limits<std::int32_t>::max();
    box.row = std::numeric_limits<std::int32_t>::min();
    return box;
}

/// Grows @p box to hold @p inner: bounds outward, the smallest offset, the
/// largest row.
void enclose(detail::SearchBox& box, const detail::SearchBox& inner) noexcept {
    for (std::size_t axis = 0; axis < 6; ++axis) {
        box.min[axis] = std::min(box.min[axis], inner.min[axis]);
        box.max[axis] = std::max(box.max[axis], inner.max[axis]);
    }
    box.offset = std::min(box.offset, inner.offset);
    box.row = std::max(box.row, inner.row);
}

/// Whether @p outer holds @p inner as a node must hold what is under it.
[[nodiscard]] bool holds(const detail::SearchBox& outer, const detail::SearchBox& inner) noexcept {
    for (std::size_t axis = 0; axis < 6; ++axis) {
        if (outer.min[axis] > inner.min[axis] || outer.max[axis] < inner.max[axis]) {
            return false;
        }
    }
    return outer.offset <= inner.offset && outer.row >= inner.row;
}

/// The row as the tree holds it. Narrowing is exact: fromEntries has held
/// every bound to ±20000 and every offset to [0, 10000] (see SearchBox), and
/// a list to kMaxEntries rows.
[[nodiscard]] detail::SearchBox boxOf(const QuantizedPoint& point, std::size_t row) noexcept {
    detail::SearchBox box;
    for (std::size_t axis = 0; axis < 6; ++axis) {
        box.min[axis] = static_cast<std::int32_t>(point.min[axis]);
        box.max[axis] = static_cast<std::int32_t>(point.max[axis]);
    }
    box.offset = static_cast<std::int32_t>(point.offset);
    box.row = static_cast<std::int32_t>(row);
    return box;
}

/// Builds the search tree, top down. Each node's rows are kept in six
/// orders at once — sorted by box centre on each axis, row index breaking
/// ties, so the build is a function of the list alone — and a split keeps
/// all six sorted by partitioning them stably, so no node sorts anything.
///
/// THIS PROJECT'S OWN, not vanilla's tree and not derived from it: what this
/// tree's shape decides is only how many rows a query scores, never which row
/// it returns (ParameterList::find).
class TreeBuilder {
public:
    TreeBuilder(const std::vector<QuantizedPoint>& points, std::vector<detail::SearchNode>& nodes,
                std::vector<detail::SearchBox>& leafRows)
        : nodes_(nodes), leafRows_(leafRows) {
        rows_.reserve(points.size());
        for (std::size_t row = 0; row < points.size(); ++row) {
            rows_.push_back(boxOf(points[row], row));
        }
        for (std::size_t axis = 0; axis < 6; ++axis) {
            std::vector<std::uint32_t>& order = orders_[axis];
            order.resize(rows_.size());
            for (std::size_t row = 0; row < rows_.size(); ++row) {
                order[row] = static_cast<std::uint32_t>(row);
            }
            std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
                const std::int64_t centreA = centre(rows_[a], axis);
                const std::int64_t centreB = centre(rows_[b], axis);
                return centreA < centreB || (centreA == centreB && a < b);
            });
        }
        inLeft_.assign(rows_.size(), 0U);
        scratch_.resize(rows_.size());
        suffixMargin_.resize(rows_.size());
    }

    /// The tree's depth, root to deepest leaf.
    [[nodiscard]] std::size_t build() {
        nodes_.clear();
        leafRows_.clear();
        if (rows_.empty()) {
            return 0;
        }
        nodes_.reserve((2 * rows_.size()) / kLeafRows + 2);
        leafRows_.reserve(rows_.size());
        nodes_.emplace_back();
        buildNode(0, 0, rows_.size(), 0);
        return depth_;
    }

private:
    struct Split {
        std::size_t axis = 0;
        /// How many rows, in that axis's order, go to the left child.
        std::size_t left = 0;
    };

    [[nodiscard]] static std::int64_t centre(const detail::SearchBox& box,
                                             std::size_t axis) noexcept {
        return std::int64_t{box.min[axis]} + std::int64_t{box.max[axis]};
    }

    void buildNode(std::size_t node, std::size_t begin, std::size_t end, std::size_t depth) {
        depth_ = std::max(depth_, depth);
        detail::SearchBox box = emptyBox();
        for (std::size_t i = begin; i < end; ++i) {
            enclose(box, rows_[orders_[0][i]]);
        }
        nodes_[node].box = box;

        const std::size_t count = end - begin;
        if (count <= kLeafRows) {
            // A leaf's rows in list order. Any order returns the same row;
            // this one keeps the tree a function of the list alone.
            std::vector<std::uint32_t> members(
                orders_[0].begin() + static_cast<std::ptrdiff_t>(begin),
                orders_[0].begin() + static_cast<std::ptrdiff_t>(end));
            std::ranges::sort(members);
            nodes_[node].first = static_cast<std::uint32_t>(leafRows_.size());
            nodes_[node].count = static_cast<std::uint32_t>(count);
            for (const std::uint32_t row : members) {
                leafRows_.push_back(rows_[row]);
            }
            return;
        }

        const Split split =
            depth < kCostSplitDepth ? cheapestSplit(begin, end) : medianSplit(begin, end);
        partition(split, begin, end);

        const std::size_t left = nodes_.size();
        nodes_[node].first = static_cast<std::uint32_t>(left);
        nodes_[node].count = 0;
        nodes_.emplace_back();
        nodes_.emplace_back();
        buildNode(left, begin, begin + split.left, depth + 1);
        buildNode(left + 1, begin + split.left, end, depth + 1);
    }

    /// The split, over every axis and every cut of that axis's order, whose
    /// children's margins weighted by their row counts sum least. Equal costs
    /// go to the more even split, then to the first axis and cut found.
    [[nodiscard]] Split cheapestSplit(std::size_t begin, std::size_t end) {
        const std::size_t count = end - begin;
        Split best;
        std::int64_t bestCost = std::numeric_limits<std::int64_t>::max();
        std::size_t bestImbalance = std::numeric_limits<std::size_t>::max();
        for (std::size_t axis = 0; axis < 6; ++axis) {
            const std::vector<std::uint32_t>& order = orders_[axis];
            // suffixMargin_[k]: the margin of the rows from the k-th on.
            detail::SearchBox suffix = emptyBox();
            for (std::size_t k = count - 1; k >= 1; --k) {
                enclose(suffix, rows_[order[begin + k]]);
                suffixMargin_[k] = margin(suffix);
            }
            detail::SearchBox prefix = emptyBox();
            for (std::size_t k = 1; k < count; ++k) {
                enclose(prefix, rows_[order[begin + k - 1]]);
                // Margins are at most 6 x 40000 and counts at most a million,
                // so the cost is far inside int64.
                const std::int64_t cost = (margin(prefix) * static_cast<std::int64_t>(k)) +
                                          (suffixMargin_[k] * static_cast<std::int64_t>(count - k));
                const std::size_t imbalance = k * 2 > count ? (k * 2) - count : count - (k * 2);
                if (cost < bestCost || (cost == bestCost && imbalance < bestImbalance)) {
                    bestCost = cost;
                    bestImbalance = imbalance;
                    best = Split{.axis = axis, .left = k};
                }
            }
        }
        return best;
    }

    /// Halves at the median of the axis whose box centres spread widest
    /// (the first such axis on a tie).
    [[nodiscard]] Split medianSplit(std::size_t begin, std::size_t end) const {
        Split best{.axis = 0, .left = (end - begin) / 2};
        std::int64_t widest = -1;
        for (std::size_t axis = 0; axis < 6; ++axis) {
            const std::vector<std::uint32_t>& order = orders_[axis];
            const std::int64_t spread =
                centre(rows_[order[end - 1]], axis) - centre(rows_[order[begin]], axis);
            if (spread > widest) {
                widest = spread;
                best.axis = axis;
            }
        }
        return best;
    }

    /// Moves the split's left rows to the front of [begin, end) in all six
    /// orders, each staying sorted within its half.
    void partition(const Split& split, std::size_t begin, std::size_t end) {
        const std::vector<std::uint32_t>& chosen = orders_[split.axis];
        for (std::size_t i = begin; i < end; ++i) {
            inLeft_[chosen[i]] = static_cast<std::uint8_t>(i < begin + split.left);
        }
        for (std::size_t axis = 0; axis < 6; ++axis) {
            if (axis == split.axis) {
                continue;
            }
            std::vector<std::uint32_t>& order = orders_[axis];
            std::size_t out = begin;
            for (std::size_t i = begin; i < end; ++i) {
                if (inLeft_[order[i]] != 0U) {
                    scratch_[out++] = order[i];
                }
            }
            for (std::size_t i = begin; i < end; ++i) {
                if (inLeft_[order[i]] == 0U) {
                    scratch_[out++] = order[i];
                }
            }
            std::copy(scratch_.begin() + static_cast<std::ptrdiff_t>(begin),
                      scratch_.begin() + static_cast<std::ptrdiff_t>(end),
                      order.begin() + static_cast<std::ptrdiff_t>(begin));
        }
    }

    std::vector<detail::SearchNode>& nodes_;
    std::vector<detail::SearchBox>& leafRows_;
    std::vector<detail::SearchBox> rows_;
    std::array<std::vector<std::uint32_t>, 6> orders_;
    std::vector<std::uint8_t> inLeft_;
    std::vector<std::uint32_t> scratch_;
    std::vector<std::int64_t> suffixMargin_;
    std::size_t depth_ = 0;
};

} // namespace

QuantizedSample QuantizedSample::of(const ClimateSample& sample) noexcept {
    return QuantizedSample{
        .temperature = quantizeCoord(sample.temperature),
        .humidity = quantizeCoord(sample.humidity),
        .continentalness = quantizeCoord(sample.continentalness),
        .erosion = quantizeCoord(sample.erosion),
        .depth = quantizeCoord(sample.depth),
        .weirdness = quantizeCoord(sample.weirdness),
    };
}

QuantizedPoint QuantizedPoint::of(const ParameterPoint& point) noexcept {
    const std::array<Parameter, 6> axes{point.temperature, point.humidity, point.continentalness,
                                        point.erosion,     point.depth,    point.weirdness};
    QuantizedPoint quantized;
    for (std::size_t axis = 0; axis < 6; ++axis) {
        quantized.min[axis] = quantizeCoord(axes[axis].min);
        quantized.max[axis] = quantizeCoord(axes[axis].max);
    }
    quantized.offset = quantizeCoord(point.offset);
    return quantized;
}

std::int64_t QuantizedPoint::fitness(const QuantizedSample& sample) const noexcept {
    const std::array<std::int64_t, 6> values = axesOf(sample);
    std::int64_t total = offset * offset;
    for (std::size_t axis = 0; axis < 6; ++axis) {
        total += axisTerm(values[axis], min[axis], max[axis]);
    }
    return total;
}

std::int64_t ParameterPoint::fitness(const ClimateSample& sample) const noexcept {
    return QuantizedPoint::of(*this).fitness(QuantizedSample::of(sample));
}

ParameterList ParameterList::fromJson(const nlohmann::json& json,
                                      const data::ResourceLocation& id) {
    if (!json.is_object() || !json.contains("biomes")) {
        fail(id, R"(must be an object with a "biomes" array)");
    }
    const nlohmann::json& biomes = json.at("biomes");
    if (!biomes.is_array()) {
        fail(id, R"("biomes" must be an array)");
    }
    if (biomes.size() > kMaxEntries) {
        fail(id, "has " + std::to_string(biomes.size()) + " entries, which is not a dimension");
    }

    std::vector<Entry> entries;
    entries.reserve(biomes.size());
    for (std::size_t row = 0; row < biomes.size(); ++row) {
        const nlohmann::json& entry = biomes[row];
        if (!entry.is_object() || !entry.contains("biome") || !entry.contains("parameters")) {
            fail(id, "entry " + std::to_string(row) + R"( needs "biome" and "parameters")");
        }
        const nlohmann::json& parameters = entry.at("parameters");
        if (!parameters.is_object()) {
            fail(id, "entry " + std::to_string(row) + R"(: "parameters" must be an object)");
        }

        Entry read;
        try {
            read.biome = data::ResourceLocation::parse(entry.at("biome").get<std::string>());
        } catch (const nlohmann::json::exception&) {
            fail(id, "entry " + std::to_string(row) + R"(: "biome" must be a string)");
        } catch (const data::ResourceLocationError& error) {
            fail(id, "entry " + std::to_string(row) + ": " + error.what());
        }

        // Every axis is required. A missing one is not a default of zero: a
        // zero range is a real and very narrow constraint, and inventing one
        // would put a biome somewhere it does not belong.
        for (const char* axis : {"temperature", "humidity", "continentalness", "erosion", "depth",
                                 "weirdness", "offset"}) {
            if (!parameters.contains(axis)) {
                fail(id, "entry " + std::to_string(row) + " has no \"" + std::string(axis) + "\"");
            }
        }

        read.parameters.temperature =
            readParameter(parameters.at("temperature"), id, "temperature", row);
        read.parameters.humidity = readParameter(parameters.at("humidity"), id, "humidity", row);
        read.parameters.continentalness =
            readParameter(parameters.at("continentalness"), id, "continentalness", row);
        read.parameters.erosion = readParameter(parameters.at("erosion"), id, "erosion", row);
        read.parameters.depth = readParameter(parameters.at("depth"), id, "depth", row);
        read.parameters.weirdness = readParameter(parameters.at("weirdness"), id, "weirdness", row);

        const nlohmann::json& offset = parameters.at("offset");
        if (!offset.is_number()) {
            fail(id, "entry " + std::to_string(row) + R"(: "offset" must be a number)");
        }
        read.parameters.offset = offset.get<double>();

        entries.push_back(std::move(read));
    }
    return fromEntries(std::move(entries), id);
}

ParameterList ParameterList::fromEntries(std::vector<Entry> entries,
                                         const data::ResourceLocation& id) {
    if (entries.size() > kMaxEntries) {
        fail(id, "has " + std::to_string(entries.size()) + " entries, which is not a dimension");
    }
    ParameterList list;
    list.quantized_.reserve(entries.size());
    for (std::size_t row = 0; row < entries.size(); ++row) {
        const ParameterPoint& point = entries[row].parameters;
        for (const auto& [axis, parameter] :
             {std::pair{"temperature", point.temperature}, std::pair{"humidity", point.humidity},
              std::pair{"continentalness", point.continentalness},
              std::pair{"erosion", point.erosion}, std::pair{"depth", point.depth},
              std::pair{"weirdness", point.weirdness}}) {
            if (parameter.min > parameter.max) {
                fail(id, std::string(axis) + " of entry " + std::to_string(row) +
                             " has its minimum above its maximum");
            }
            // The schema's range, which also keeps quantizeCoord defined, NaN
            // refused with the rest.
            for (const double bound : {parameter.min, parameter.max}) {
                if (std::isnan(bound) || bound < -kClimateBoundLimit ||
                    bound > kClimateBoundLimit) {
                    fail(id, std::string(axis) + " of entry " + std::to_string(row) + " is " +
                                 spell(bound) + ", outside the schema's range [-2, 2]");
                }
            }
        }
        if (std::isnan(point.offset) || point.offset < 0.0 || point.offset > kMaxClimateOffset) {
            fail(id, "offset of entry " + std::to_string(row) + " is " + spell(point.offset) +
                         ", outside the schema's range [0, 1]");
        }
        list.quantized_.push_back(QuantizedPoint::of(point));
    }
    list.entries_ = std::move(entries);
    list.buildSearchTree();
    return list;
}

namespace {

/// How many median halvings take @p rows down to a leaf's worth.
[[nodiscard]] constexpr std::size_t halvingsToLeaf(std::size_t rows) noexcept {
    std::size_t levels = 0;
    while (rows > kLeafRows) {
        rows = (rows + 1) / 2;
        ++levels;
    }
    return levels;
}

} // namespace

void ParameterList::buildSearchTree() {
    // The pending stack in searchTree holds at most one entry per level plus
    // one, so a fixed array of kMaxSearchDepth + 1 suffices if no tree is
    // deeper than kMaxSearchDepth — and none is: cost splits stop at
    // kCostSplitDepth, and median splits from any size reach a leaf within
    // halvingsToLeaf(kMaxEntries).
    static_assert(kCostSplitDepth + halvingsToLeaf(kMaxEntries) <= kMaxSearchDepth);
    TreeBuilder builder(quantized_, searchNodes_, searchRows_);
    searchDepth_ = builder.build();
    searchNodes_.shrink_to_fit();
}

template<bool Counting>
std::size_t ParameterList::searchTree(const QuantizedSample& sample, const std::size_t hint,
                                      SearchCounts* counts) const noexcept {
    const std::array<std::int64_t, 6> values = axesOf(sample);

    // The best row so far and its fitness, starting from no row at all. Not
    // a large sentinel short of the maximum: a sample near
    // ±kMaxQuantizedSample has a real fitness near 6e18, past 2^62.
    std::int64_t bestFitness = std::numeric_limits<std::int64_t>::max();
    std::int64_t bestRow = -1;
    if (hint < quantized_.size()) {
        // The warm start: the hinted row scored first, as though it were the
        // first row met (findRowFrom says why that cannot change the row).
        // QuantizedPoint::fitness is score() over the same integers — the
        // tree's rows carry exactly these bounds (auditSearchTree) — so it is
        // the value the row scores in its leaf too, where it then neither
        // beats nor ties-and-wins against itself.
        bestFitness = quantized_[hint].fitness(sample);
        bestRow = static_cast<std::int64_t>(hint);
        if constexpr (Counting) {
            ++counts->rows;
        }
    }

    // Every row under a node scores at least its bound. Above the best, none
    // can win; equal to it, one can only tie, and a tie goes to the later
    // row, so a node is skipped only when every row in it (`box.row` is the
    // largest) is earlier than the best. Strict on both counts: a node whose
    // bound equals the best and that holds a row later than it is entered.
    // Once true for a node this stays true, because the best only improves
    // (less fitness, or equal fitness and a later row): so a node skipped
    // when it is first scored would be skipped at any later point too.
    const auto skipped = [&](const std::int64_t bound, const std::int32_t lastRow) noexcept {
        return bound > bestFitness || (bound == bestFitness && lastRow < bestRow);
    };

    // Nodes still to visit, each with its lower bound. Depth first: of a
    // node's two children the more promising is entered at once and the
    // other waits here, so at most one entry per level waits under the node
    // being entered, hence the bound (buildSearchTree). Only the entries
    // below `top` are ever read, so the array is left uninitialised.
    struct Pending {
        std::uint32_t node;
        std::int64_t bound;
    };

    std::array<Pending, kMaxSearchDepth + 1> pending;
    std::size_t top = 0;
    std::uint32_t current = 0;
    {
        const std::int64_t rootBound = score(searchNodes_[0].box, values);
        if constexpr (Counting) {
            ++counts->nodes;
        }
        if (skipped(rootBound, searchNodes_[0].box.row)) {
            return static_cast<std::size_t>(bestRow);
        }
    }

    while (true) {
        const detail::SearchNode& node = searchNodes_[current];
        if (node.count == 0) {
            const std::uint32_t left = node.first;
            const std::uint32_t right = node.first + 1;
            const std::int64_t leftBound = score(searchNodes_[left].box, values);
            const std::int64_t rightBound = score(searchNodes_[right].box, values);
            if constexpr (Counting) {
                counts->nodes += 2;
            }
            const std::int32_t leftRow = searchNodes_[left].box.row;
            const std::int32_t rightRow = searchNodes_[right].box.row;
            // Enter the lower bound first; on equal bounds, the child holding
            // the later rows, which win the ties. Either order returns the
            // same row.
            const bool leftFirst =
                leftBound < rightBound || (leftBound == rightBound && leftRow > rightRow);
            const std::uint32_t first = leftFirst ? left : right;
            const std::uint32_t second = leftFirst ? right : left;
            const std::int64_t firstBound = leftFirst ? leftBound : rightBound;
            const std::int64_t secondBound = leftFirst ? rightBound : leftBound;
            if (!skipped(secondBound, leftFirst ? rightRow : leftRow)) {
                pending[top++] = Pending{.node = second, .bound = secondBound};
            }
            if (!skipped(firstBound, leftFirst ? leftRow : rightRow)) {
                current = first;
                continue;
            }
        } else {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const detail::SearchBox& row = searchRows_[i];
                const std::int64_t fitness = score(row, values);
                // The scan's order: less fitness, or equal and later. A
                // strict total order over rows, so the order rows are scored
                // in cannot change which one is kept.
                if (fitness < bestFitness || (fitness == bestFitness && row.row > bestRow)) {
                    bestFitness = fitness;
                    bestRow = row.row;
                }
            }
            if constexpr (Counting) {
                counts->rows += node.count;
            }
        }
        // The next waiting node the best has not since ruled out — rechecked
        // here, because the best may have improved since it was pushed.
        bool resumed = false;
        while (top > 0) {
            const Pending next = pending[--top];
            if (!skipped(next.bound, searchNodes_[next.node].box.row)) {
                current = next.node;
                resumed = true;
                break;
            }
        }
        if (!resumed) {
            break;
        }
    }
    return static_cast<std::size_t>(bestRow);
}

std::size_t ParameterList::scanLinear(const QuantizedSample& sample) const noexcept {
    std::size_t best = 0;
    std::int64_t bestFitness = quantized_[0].fitness(sample);
    for (std::size_t i = 1; i < quantized_.size(); ++i) {
        const std::int64_t fitness = quantized_[i].fitness(sample);
        // Less *or equal*: a tie takes the later entry. See the header — this
        // is measured against vanilla, not derived, and it decides a lot of
        // cells because quantising makes near-misses into exact ties.
        if (fitness <= bestFitness) {
            bestFitness = fitness;
            best = i;
        }
    }
    return best;
}

const data::ResourceLocation& ParameterList::find(const ClimateSample& sample) const {
    return entries_[findRow(sample)].biome;
}

std::size_t ParameterList::findRow(const ClimateSample& sample) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return searchTree<false>(QuantizedSample::of(sample), kNoHint, nullptr);
}

std::size_t ParameterList::findRow(const QuantizedSample& sample) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return searchTree<false>(sample, kNoHint, nullptr);
}

std::size_t ParameterList::findRow(const QuantizedSample& sample, SearchCounts& counts) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return searchTree<true>(sample, kNoHint, &counts);
}

std::size_t ParameterList::findRowFrom(const ClimateSample& sample, const std::size_t hint) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return searchTree<false>(QuantizedSample::of(sample), hint, nullptr);
}

std::size_t ParameterList::findRowFrom(const QuantizedSample& sample,
                                       const std::size_t hint) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return searchTree<false>(sample, hint, nullptr);
}

std::size_t ParameterList::findRowFrom(const QuantizedSample& sample, const std::size_t hint,
                                       SearchCounts& counts) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return searchTree<true>(sample, hint, &counts);
}

std::size_t ParameterList::findRowLinear(const ClimateSample& sample) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return scanLinear(QuantizedSample::of(sample));
}

std::size_t ParameterList::findRowLinear(const QuantizedSample& sample) const {
    checkNotEmpty(entries_);
    checkSamples(sample);
    return scanLinear(sample);
}

SearchTreeShape ParameterList::searchTreeShape() const noexcept {
    SearchTreeShape shape;
    shape.nodes = searchNodes_.size();
    for (const detail::SearchNode& node : searchNodes_) {
        shape.leaves += node.count != 0 ? 1U : 0U;
    }
    shape.depth = searchDepth_;
    shape.bytes = (searchNodes_.capacity() * sizeof(detail::SearchNode)) +
                  (searchRows_.capacity() * sizeof(detail::SearchBox));
    return shape;
}

std::string ParameterList::auditSearchTree() const {
    if (entries_.empty()) {
        return searchNodes_.empty() && searchRows_.empty() ? std::string{}
                                                           : "an empty list has a search tree";
    }
    if (searchNodes_.empty()) {
        return "a list of " + std::to_string(entries_.size()) + " rows has no search tree";
    }
    std::vector<std::uint8_t> seen(entries_.size(), 0U);
    std::size_t rowsSeen = 0;
    // Each node with its depth, from the root; a node's children are checked
    // against it, so containment holds transitively from every node down to
    // every row under it.
    std::vector<std::pair<std::size_t, std::size_t>> open{{0, 0}};
    std::size_t visited = 0;
    while (!open.empty()) {
        const auto [index, depth] = open.back();
        open.pop_back();
        if (++visited > searchNodes_.size()) {
            return "the search tree is not a tree: a node is reached twice";
        }
        if (depth > kMaxSearchDepth) {
            return "the search tree is deeper than " + std::to_string(kMaxSearchDepth);
        }
        const detail::SearchNode& node = searchNodes_[index];
        const std::string where = "search tree node " + std::to_string(index);
        if (node.count == 0) {
            if (std::size_t{node.first} + 1 >= searchNodes_.size()) {
                return where + " names children past the end";
            }
            for (const std::uint32_t child : {node.first, node.first + 1}) {
                if (!holds(node.box, searchNodes_[child].box)) {
                    return where + " does not hold its child " + std::to_string(child);
                }
                open.emplace_back(child, depth + 1);
            }
            continue;
        }
        if (std::size_t{node.first} + node.count > searchRows_.size()) {
            return where + " names rows past the end";
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const detail::SearchBox& row = searchRows_[i];
            if (row.row < 0 || static_cast<std::size_t>(row.row) >= entries_.size()) {
                return where + " holds a row index outside the list";
            }
            const auto rowIndex = static_cast<std::size_t>(row.row);
            if (seen[rowIndex] != 0U) {
                return "row " + std::to_string(rowIndex) + " is in the search tree twice";
            }
            seen[rowIndex] = 1U;
            ++rowsSeen;
            const QuantizedPoint& point = quantized_[rowIndex];
            for (std::size_t axis = 0; axis < 6; ++axis) {
                if (row.min[axis] != point.min[axis] || row.max[axis] != point.max[axis]) {
                    return "row " + std::to_string(rowIndex) + " carries other bounds in the tree";
                }
            }
            if (row.offset != point.offset) {
                return "row " + std::to_string(rowIndex) + " carries another offset in the tree";
            }
            if (!holds(node.box, row)) {
                return where + " does not hold its row " + std::to_string(rowIndex);
            }
        }
    }
    if (rowsSeen != entries_.size() || searchRows_.size() != entries_.size()) {
        return "the search tree holds " + std::to_string(rowsSeen) + " of the list's " +
               std::to_string(entries_.size()) + " rows";
    }
    return {};
}

} // namespace stratum::biome
