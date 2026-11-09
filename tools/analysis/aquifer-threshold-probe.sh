#!/usr/bin/env bash
# Stratum — the aquifer's thresholds in their last bits: Q5.6's level rule and
# Q5.9's deep-dark override, at the values where the engine's floating-point
# form and the decimal reading part.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-threshold-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. IMPLEMENTER-RULING-01 (aquifer P1) adopts two
# floating-point amendments of spec/aquifer-spec.md, and its §2 admits a claim
# to lib/ only once a golden confirms it. No golden or probe holds a value
# within the last bits of a threshold, so none could tell the forms apart.
# This puts router constants exactly there:
#
#   Q5.6. With phi 0 (here: off the ramp, the surface far above every cell's
#   centre), the engine's thresholds are -0.3 + 1 * (0.8 - (-0.3)) = 0.8 and
#   -0.8 + 1 * (0.4 - (-0.8)) = 0.40000000000000013, where the decimal
#   reading has 0.8 and 0.4. Floodedness:
#     tc  0.5                    control: the ladder under either reading
#     tb  0.4000000000000001     one ulp above 0.4: the ladder under the
#                                decimal reading, DRY in the engine's
#     te  0.40000000000000013    the engine's threshold itself: dry (strict)
#     ta  0.4000000000000002     one ulp above it: the ladder under both
#
#   Q5.9. Floodedness 0.9 (the sea, unless the override forces it dry):
#     de  erosion -0.225, depth 1.0
#     dd  erosion -0.5,   depth 0.9
#                                past the engine's widened single-precision
#                                thresholds (-0.22499999403953552,
#                                0.8999999761581421) and AT the decimal ones:
#                                DRY in the engine, the sea in the decimal
#     dc  erosion -0.22499999403953552, depth 1.0
#                                AT the engine's erosion threshold: not deep
#                                dark, so the sea under both
#
# Everything else is the far-cell probe's constant world (aquifer-farcell-
# probe.sh): open void, barrier -2.0, spread 0.0, lava -1.0, surface 96, sea
# 63, water, no surface rule that fires, Xoroshiro. A block is the lattice's
# decision alone.
#
# Read by tests/conformance/vanilla_aquifer_threshold_test.cpp. Nothing this
# writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:-42}"
[[ "${seed}" =~ ^-?[0-9]+$ ]] || { echo "seed must be a whole number: ${seed}" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/aqthreshold_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, math, sys

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

ENGINE_LOW = -0.8 + ((0.0 - 1.0) / (0.0 - 1.0)) * (0.4 - (-0.8))
assert ENGINE_LOW == 0.40000000000000013, ENGINE_LOW


def aquifer(name, floodedness, erosion=0.0, depth=0.0):
    return {"name": name, "min_y": -64, "height": 384,
            "legacy_random_source": False,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "ore_veins_enabled": False, "sea_level": 63,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0,
                       "fluid_level_floodedness": floodedness,
                       "fluid_level_spread": 0.0,
                       "lava": -1.0,
                       "erosion": erosion,
                       "depth": depth,
                       "preliminary_surface_level": 96.0}}


spec = [
    aquifer("tc", 0.5),
    aquifer("tb", math.nextafter(0.4, 1.0)),
    aquifer("te", ENGINE_LOW),
    aquifer("ta", math.nextafter(ENGINE_LOW, 1.0)),
    aquifer("de", 0.9, erosion=-0.225, depth=1.0),
    aquifer("dd", 0.9, erosion=-0.5, depth=0.9),
    aquifer("dc", 0.9, erosion=-0.22499999403953552, depth=1.0),
]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
