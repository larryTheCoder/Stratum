// Stratum — one dimension of one world, compiled from its frozen pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The single place a frozen pipeline (SPEC §6) becomes something that fills
// chunks: the noise registry for a seed, the surface rule graph, the
// ChunkFiller, and the biome search at vanilla's quart resolution. Every
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
#include <span>
#include <stdexcept>

namespace stratum::world {

/// A dimension this build cannot compile from the pipeline it was given:
/// named settings or a named biome parameter list the pipeline does not
/// carry. Loaders and the filler raise their own errors, unchanged.
class DimensionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Immutable once compiled and safe to share between threads (SPEC §4.1):
/// `fillBlocks` and `fillBiomes` take all their scratch state from the
/// caller or build it per call.
class CompiledDimension {
public:
    /// Compiles the noise settings @p noiseSettings of @p pipeline, choosing
    /// biomes from its parameter list @p biomeParameterList, for
    /// @p worldSeed. Both are named rather than derived because a pipeline
    /// does not yet record which list a dimension uses (vanilla's world
    /// presets say; the freeze does not carry them).
    ///
    /// Everything is taken from the settings as they were frozen, ore veins
    /// included: their three draws are derived and confirmed per block
    /// against the server (SPEC's M3 section), so this no longer forces the
    /// flag off. Throws DimensionError for a name the pipeline does not
    /// carry; a legacy-random-source dimension is refused by the noise
    /// registry, naming it.
    [[nodiscard]] static std::unique_ptr<CompiledDimension>
    compile(freeze::Pipeline pipeline, const data::ResourceLocation& noiseSettings,
            const data::ResourceLocation& biomeParameterList, std::int64_t worldSeed);

    ~CompiledDimension();
    CompiledDimension(const CompiledDimension&) = delete;
    CompiledDimension& operator=(const CompiledDimension&) = delete;
    CompiledDimension(CompiledDimension&&) = delete;
    CompiledDimension& operator=(CompiledDimension&&) = delete;

    [[nodiscard]] const settings::NoiseGeometry& geometry() const noexcept;

    /// The Java block states of chunk (@p chunkX, @p chunkZ). @p into must
    /// have been built for `geometry()`.
    void fillBlocks(std::int32_t chunkX, std::int32_t chunkZ, terrain::ChunkBuffer& into) const;

    /// The Java biome of every quart of the chunk, at vanilla's storage
    /// resolution: `into[(qy * 4 + qz) * 4 + qx]`, qy counted up from
    /// `geometry().minY`, so `into` holds `16 * (height / 4)` entries. The
    /// pointers stay valid for this dimension's lifetime. Throws
    /// DimensionError if @p into is the wrong size.
    void fillBiomes(std::int32_t chunkX, std::int32_t chunkZ,
                    std::span<const data::ResourceLocation*> into) const;

private:
    CompiledDimension();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stratum::world
