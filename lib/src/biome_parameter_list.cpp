// Stratum — the multi-noise biome parameter table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
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
    const std::array<std::int64_t, 6> values{sample.temperature,     sample.humidity,
                                             sample.continentalness, sample.erosion,
                                             sample.depth,           sample.weirdness};
    std::int64_t total = offset * offset;
    for (std::size_t axis = 0; axis < 6; ++axis) {
        // How far the sample lies outside the range — zero when inside.
        const std::int64_t value = values[axis];
        std::int64_t distance = 0;
        if (value < min[axis]) {
            distance = min[axis] - value;
        } else if (value > max[axis]) {
            distance = value - max[axis];
        }
        total += distance * distance;
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

    ParameterList list;
    list.entries_.reserve(biomes.size());
    list.quantized_.reserve(biomes.size());
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

        list.quantized_.push_back(QuantizedPoint::of(read.parameters));
        list.entries_.push_back(std::move(read));
    }
    return list;
}

const data::ResourceLocation& ParameterList::find(const ClimateSample& sample) const {
    if (entries_.empty()) {
        throw ParameterError("this dimension's biome parameter list is empty, so there is no "
                             "biome to choose; refusing rather than inventing one");
    }

    const QuantizedSample quantized = QuantizedSample::of(sample);
    std::size_t best = 0;
    std::int64_t bestFitness = quantized_[0].fitness(quantized);
    for (std::size_t i = 1; i < quantized_.size(); ++i) {
        const std::int64_t fitness = quantized_[i].fitness(quantized);
        // Less *or equal*: a tie takes the later entry. See the header — this
        // is measured against vanilla, not derived, and it decides a lot of
        // cells because quantising makes near-misses into exact ties.
        if (fitness <= bestFitness) {
            bestFitness = fitness;
            best = i;
        }
    }
    return entries_[best].biome;
}

} // namespace stratum::biome
