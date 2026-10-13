// Stratum — a `worldgen/noise` entry, as declared.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Its own header because two layers need it and neither should have to
// include the other. A density function's `noise` field may either name a
// `worldgen/noise` entry or carry its parameters inline — mcdoc declares it
// as `#[id="worldgen/noise"] string | NoiseParameters` — so the resolved
// graph has to be able to hold one, while building a sampleable noise from
// one belongs to the registry and needs the world seed.

#pragma once

#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string_view>
#include <vector>

namespace stratum::density {

/// Raised when a `worldgen/noise` entry is missing or malformed. Never
/// silently substituted: a noise that quietly defaulted would produce a
/// world that generates but is not the one asked for (SPEC §8).
class NoiseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A `worldgen/noise` entry, as declared.
struct NoiseParameters {
    /// Vanilla's `firstOctave`, the exponent of the lowest frequency.
    int firstOctave = 0;
    /// One amplitude per octave. Zeroes are meaningful: they position the
    /// octaves that follow without contributing themselves.
    std::vector<double> amplitudes;

    /// Parses one entry. @p id names the entry in any error raised.
    [[nodiscard]] static NoiseParameters fromJson(const nlohmann::json& json,
                                                  const data::ResourceLocation& id);

    /// Parses a noise written inline in a density function, where there is
    /// no identifier to name it by. @p where says which field carried it,
    /// since that is all a reader of the error has to go on.
    [[nodiscard]] static NoiseParameters fromInline(const nlohmann::json& json,
                                                    std::string_view where);

    /// The bounds both parsers hold a noise to — `firstOctave` in
    /// [-64, 64], between 1 and 64 amplitudes — as one check, so that a
    /// frozen pipeline's noises are held to them too (freeze::read): a
    /// resealed blob with a `firstOctave` of 2^31 - 1 overflowed the octave
    /// name `octave_<firstOctave + i>` (SPEC §8). Throws NoiseError, with
    /// @p subject leading the message.
    void check(std::string_view subject) const;

    /// Written out rather than defaulted so that the amplitude comparison
    /// happens inside <vector> — the project builds with -Wfloat-equal, and
    /// this is a comparison of two declarations rather than of two computed
    /// values.
    [[nodiscard]] bool operator==(const NoiseParameters& other) const {
        return firstOctave == other.firstOctave && amplitudes == other.amplitudes;
    }
};

} // namespace stratum::density
