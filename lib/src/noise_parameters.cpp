// Stratum — a `worldgen/noise` entry, as declared.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_parameters.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stratum::density {

namespace {

/// The range vanilla's own noises occupy is far narrower than this; the
/// bound exists so that a nonsense `firstOctave` is refused by name rather
/// than turning into a frequency of zero or infinity inside ldexp.
constexpr int kMinFirstOctave = -64;
constexpr int kMaxFirstOctave = 64;

/// Amplitude lists are short — vanilla's longest is nine. A pack asking for
/// thousands is either corrupt or hostile, and either way should be told so.
constexpr std::size_t kMaxAmplitudes = 64;

/// Both spellings of a noise are the same shape, so both are read here and
/// only the subject of the error message differs. @p subject is written into
/// every refusal, because "has no firstOctave" without saying whose is not
/// something anyone can act on.
[[nodiscard]] NoiseParameters parse(const nlohmann::json& json, std::string_view subject) {
    const auto fail = [subject](const std::string& what) {
        throw NoiseError(std::string(subject) + ": " + what);
    };

    if (!json.is_object()) {
        fail("must be an object, not " + std::string(json.type_name()));
    }
    if (!json.contains("firstOctave")) {
        fail(R"(has no "firstOctave")");
    }
    if (!json.contains("amplitudes")) {
        fail(R"(has no "amplitudes")");
    }

    const nlohmann::json& firstOctave = json.at("firstOctave");
    // number_integer covers both signed and unsigned; a float here would be
    // an exponent with a fractional part, which means the pack is wrong
    // about something rather than merely imprecise.
    if (!firstOctave.is_number_integer()) {
        fail(R"("firstOctave" must be an integer, not )" + std::string(firstOctave.type_name()));
    }
    const auto octave = firstOctave.get<std::int64_t>();
    if (octave < kMinFirstOctave || octave > kMaxFirstOctave) {
        fail(R"("firstOctave" is )" + std::to_string(octave) + ", outside the supported range [" +
             std::to_string(kMinFirstOctave) + ", " + std::to_string(kMaxFirstOctave) + "]");
    }

    const nlohmann::json& amplitudes = json.at("amplitudes");
    if (!amplitudes.is_array()) {
        fail(R"("amplitudes" must be an array, not )" + std::string(amplitudes.type_name()));
    }
    if (amplitudes.empty()) {
        fail(R"("amplitudes" is empty; a noise with no octaves samples to nothing)");
    }
    if (amplitudes.size() > kMaxAmplitudes) {
        fail(R"("amplitudes" has )" + std::to_string(amplitudes.size()) + " entries, more than " +
             std::to_string(kMaxAmplitudes));
    }

    NoiseParameters parsed;
    parsed.firstOctave = static_cast<int>(octave);
    parsed.amplitudes.reserve(amplitudes.size());
    for (std::size_t i = 0; i < amplitudes.size(); ++i) {
        const nlohmann::json& amplitude = amplitudes[i];
        if (!amplitude.is_number()) {
            fail("amplitude " + std::to_string(i) + " is not a number");
        }
        parsed.amplitudes.push_back(amplitude.get<double>());
    }
    return parsed;
}

} // namespace

NoiseParameters NoiseParameters::fromJson(const nlohmann::json& json,
                                          const data::ResourceLocation& id) {
    return parse(json, "noise '" + id.toString() + "'");
}

NoiseParameters NoiseParameters::fromInline(const nlohmann::json& json, std::string_view where) {
    return parse(json, where);
}

} // namespace stratum::density
