// Stratum — what a probe region holds besides its blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Every density probe (tools/analysis/density-probe.sh) force-loads an 8x8
// window from chunk (0, 0). The window and the ring of chunks around it tick:
// their fluids flow (frozen, a small remnant still does — SPEC §7) and their
// post-processing lists empty. Every other chunk of r.0.0 that got as far as
// the noise stage — the 63 out to chunk 11, and on some worlds more beyond
// them (the water/lava probes' sea -70 arms hold 84 and 91 in all) — never
// ticked, so its blocks and its `PostProcessing` list are exactly what
// generation left. The aquifer cases score those exactly and the ticked ones
// with fluid flow allowed for (support/fluid_flow.hpp).
//
// And the probe's own noise, rebuilt from the manifest the probe wrote, for a
// case that needs a field's value where no readout reaches.
#pragma once

#include <stratum/javamath.hpp>
#include <stratum/nbt/tag.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace stratum::test {

/// The window the probe force-loaded (chunks 0..7) and the ring around it:
/// every chunk here ticked and emptied its post-processing list.
[[nodiscard]] inline bool ticked(std::int32_t cx, std::int32_t cz) {
    return cx < 9 && cz < 9;
}

/// A chunk whose noise stage ran and which never ticked: its blocks and its
/// post-processing list are generation's own.
[[nodiscard]] inline bool untouched(std::int32_t cx, std::int32_t cz, const std::string& status) {
    return !ticked(cx, cz) && (status == "minecraft:full" || status == "minecraft:carvers" ||
                               status == "minecraft:initialize_light");
}

/// A position inside one chunk, as (localX, y, localZ).
using LocalPosition = std::tuple<int, int, int>;

/// A chunk's `PostProcessing` marks, as (localX, y, localZ): one list per
/// section from the world floor up, each entry a packed `x | y << 4 | z << 8`
/// (measured: every entry of 66 156 lands on a fluid under that packing,
/// against a third on air or stone under the y/z swap).
[[nodiscard]] inline std::set<LocalPosition> postProcessingMarks(const nbt::Tag& root,
                                                                 std::int32_t minY) {
    std::set<LocalPosition> marks;
    const nbt::Tag* list = root.find("PostProcessing");
    if (list == nullptr) {
        return marks;
    }
    std::int32_t sectionY = javamath::floorDiv(minY, 16);
    for (const nbt::Tag& section : list->asList().elements) {
        for (const nbt::Tag& entry : section.asList().elements) {
            const auto packed = static_cast<std::uint16_t>(entry.asShort());
            const auto field = [packed](unsigned shift) {
                return static_cast<int>((packed >> shift) & 15U);
            };
            marks.emplace(field(0U), (sectionY * 16) + field(4U), field(8U));
        }
        ++sectionY;
    }
    return marks;
}

/// The probe's own noise, rebuilt from the manifest the probe wrote — one
/// octave, so nothing about the replica is in doubt.
[[nodiscard]] inline noise::NormalNoise probeNoise(const std::filesystem::path& root) {
    const auto manifest = nlohmann::json::parse(std::ifstream(root / "manifest.json"));
    const auto& declared = manifest.at("probe_noise");
    auto random = rng::XoroshiroPositionalFactory(manifest.at("seed").get<std::int64_t>())
                      .fromHashOf(declared.at("id").get<std::string>());
    const auto amplitudes = declared.at("amplitudes").get<std::vector<double>>();
    return noise::NormalNoise::create(random, declared.at("first_octave").get<int>(), amplitudes);
}

} // namespace stratum::test
