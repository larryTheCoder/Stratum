// Stratum — one dimension of one world, compiled from its frozen pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The single place a frozen pipeline (SPEC §6) becomes something that fills
// chunks: the noise registry for a seed, the surface rule graph, the
// ChunkFiller, and the dimension's biome source at vanilla's quart
// resolution — multi-noise or the End's (biome/source.hpp). Every
// native binding (`ext/`, `ext-nukkit/`) generates through this, so what a
// world looks like cannot depend on which server hosts it.
//
// It exists as one component rather than a copy per binding for a reason
// the first copy learned the hard way: `terrain::ChunkFiller::compile` keeps
// POINTERS into the surface rules, biome parameters and temperatures it is
// given. Build any of those as a local and move it afterwards and the filler
// reads a stack frame that no longer exists — it ran, sometimes, and failed
// an assertion deep inside the interpreter other times. Here every such
// object is a member built in place, in dependency order, before the filler
// that points into it.
//
// Output is Java-edition only — block states and biome identifiers. Mapping
// to a platform happens downstream (`lib/mapping/`, then the binding), after
// the conformance boundary (SPEC §7, §9).
#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace stratum::world {

/// A dimension this build cannot compile from the pipeline it was given:
/// named settings or a named biome parameter list the pipeline does not
/// carry, or a biome source type this build does not implement. Loaders and
/// the filler raise their own errors, unchanged.
class DimensionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The `biome_source` a dimension's generator declares, by name.
///
/// A caller names it because nothing else can: a frozen pipeline does not
/// record which source a dimension uses (vanilla's world presets and
/// dimension definitions say, and the freeze does not carry them, SPEC §6),
/// and noise settings do not imply one — vanilla pairs `minecraft:end` with
/// `minecraft:the_end` and a datapack may pair it with anything. So the TYPE
/// is part of the name. Before this, a dimension took a parameter list
/// name and nothing else, and the only way to compile the End was to hand it
/// another dimension's list: a world that generated, with plausible biomes,
/// all of them wrong (SPEC §8's severe class).
class BiomeSource {
public:
    enum class Type : std::uint8_t {
        /// `minecraft:multi_noise`, with the parameter list it searches.
        MultiNoise,
        /// `minecraft:the_end`, which has no fields.
        TheEnd,
    };

    /// `{"type": "minecraft:multi_noise", "preset": <parameterList>}`.
    [[nodiscard]] static BiomeSource multiNoise(data::ResourceLocation parameterList);

    /// `{"type": "minecraft:the_end"}`.
    [[nodiscard]] static BiomeSource theEnd();

    /// One string, for a binding whose callers pass text: `minecraft:the_end`
    /// (or `the_end`) is the End's source, and any other identifier is the
    /// multi-noise parameter list of that name, as this argument always was.
    /// The other source types vanilla defines — `minecraft:fixed`,
    /// `minecraft:checkerboard`, and `minecraft:multi_noise` named bare, with
    /// no list — are refused by name (DimensionError), never read as a list
    /// that happens to be missing. Vanilla's parameter lists are its two
    /// built-in presets, `minecraft:overworld` and `minecraft:nether`, so no
    /// list a pipeline can carry is shadowed by those four names. Throws
    /// data::ResourceLocationError for text that is no identifier at all.
    [[nodiscard]] static BiomeSource parse(std::string_view spelling);

    [[nodiscard]] Type type() const noexcept { return type_; }

    /// The parameter list a multi-noise source searches; empty for the End's.
    [[nodiscard]] const std::optional<data::ResourceLocation>& parameterList() const noexcept {
        return parameterList_;
    }

    /// What parse() reads back as this source: the list's id, or
    /// `minecraft:the_end`.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] bool operator==(const BiomeSource&) const = default;

private:
    BiomeSource(Type type, std::optional<data::ResourceLocation> parameterList)
        : type_(type), parameterList_(std::move(parameterList)) {}

    Type type_;
    std::optional<data::ResourceLocation> parameterList_;
};

/// Immutable once compiled and safe to share between threads (SPEC §4.1):
/// `fillBlocks` and `fillBiomes` take all their scratch state from the
/// caller or build it per call.
class CompiledDimension {
public:
    /// Compiles the noise settings @p noiseSettings of @p pipeline, choosing
    /// biomes through @p biomeSource, for @p worldSeed. Both are named
    /// rather than derived because a pipeline does not record which a
    /// dimension uses (BiomeSource says why).
    ///
    /// Everything is taken from the settings as they were frozen, ore veins
    /// included: their three draws are derived and confirmed per block
    /// against the server (SPEC's M3 section), so this no longer forces the
    /// flag off. Throws DimensionError for a name the pipeline does not
    /// carry; a legacy-random-source dimension is refused by the noise
    /// registry, naming it; a router entry the biome source reads that this
    /// build cannot evaluate is refused here, by the interpreter, naming it.
    [[nodiscard]] static std::unique_ptr<CompiledDimension>
    compile(freeze::Pipeline pipeline, const data::ResourceLocation& noiseSettings,
            const BiomeSource& biomeSource, std::int64_t worldSeed);

    ~CompiledDimension();
    CompiledDimension(const CompiledDimension&) = delete;
    CompiledDimension& operator=(const CompiledDimension&) = delete;
    CompiledDimension(CompiledDimension&&) = delete;
    CompiledDimension& operator=(CompiledDimension&&) = delete;

    [[nodiscard]] const settings::NoiseGeometry& geometry() const noexcept;

    /// The Java block states of chunk (@p chunkX, @p chunkZ). @p into must
    /// have been built for `geometry()`. A chunk outside
    /// density::kMaxChunkCoordinate, or one whose reads carry a noise past
    /// its 32-bit lattice (vanilla's `jagged` does, about 2 812 333 blocks
    /// out), is refused by name rather than generated (SPEC §8).
    void fillBlocks(std::int32_t chunkX, std::int32_t chunkZ, terrain::ChunkBuffer& into) const;

    /// The Java biome of every quart of the chunk over the dimension's noise
    /// height, at vanilla's storage resolution: `into[(qy * 4 + qz) * 4 +
    /// qx]`, qy counted up from `geometry().minY`, so `into` holds
    /// `16 * (height / 4)` entries — from the biome source this dimension
    /// was compiled with (biome::Source::fillChunk). The pointers stay valid
    /// for this dimension's lifetime. Throws DimensionError if @p into is
    /// the wrong size.
    void fillBiomes(std::int32_t chunkX, std::int32_t chunkZ,
                    std::span<const data::ResourceLocation*> into) const;

    /// The same for any run of quart layers: `into.size() / 16` of them,
    /// counted up from quart layer @p minQuartY (block y `4 * minQuartY`),
    /// in the same order.
    ///
    /// The noise height is not the column the server stores biomes over: a
    /// Java chunk carries one for every section of its DIMENSION TYPE, and
    /// the End's (and the Nether's) is y 0..255 where its noise settings
    /// generate 0..127. A biome source answers at any y, so this reads it
    /// above and below the noise height as at it — which over the End's
    /// upper half is what the server stores, quart for quart
    /// (tests/conformance/vanilla_end_biome_source_test.cpp). Throws
    /// DimensionError if @p into is not a whole number of layers, and
    /// std::out_of_range for a chunk or layers outside the 32-bit domain
    /// (biome::Source::fillChunk; SPEC §8).
    void fillBiomes(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t minQuartY,
                    std::span<const data::ResourceLocation*> into) const;

private:
    CompiledDimension();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stratum::world
