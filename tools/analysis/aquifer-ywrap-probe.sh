#!/usr/bin/env bash
# Stratum — the aquifer's lattice points at the edges of the 12-bit y field:
# Q3.3's width amendment, where a point's y is retained as a signed 12-bit
# value and the two cells that straddle the edge of that range wrap.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-ywrap-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. IMPLEMENTER-RULING-01 (aquifer P2, Q3.3) asks that the
# wrap be reproduced, and its §2 admits it to lib/ only once a golden
# confirms it. spec/aquifer-spec.md's Q3.3 amendment: a point is retained
# with y = ((12j + v + 2048) mod 4096) - 2048, and every later use — the
# nearest-four distance, the status position — reads the retained value.
# The identity holds for 12j + v in [-2048, 2047], so only a dimension that
# reaches past y 2027 or below y -2029 can see it:
#
#   top     cell j = 170 holds points at 2040 + v; v = 8 (y 2048) is retained
#           at -2048, about 4 090 blocks away, so it never ranks among the
#           nearest four for the blocks at y 2027 .. 2031 that consult it.
#   bottom  cell j = -171 holds points at -2052 + v; v <= 3 are retained at
#           +2044 .. +2047, likewise never near the blocks at y <= -2030.
#
# One dimension reaches both: min_y -2032, height 4064 (top 2031), the
# largest a dimension type allows. Everything is open (final_density -1), so
# every block is the aquifer's decision. The surface sits at 2200, above the
# top, so every cell is underground and takes its level from the ladder.
# The router is the deep-floor probe's (aquifer-deepfloor-probe.sh): the real
# barrier noise, floodedness 0.6 (strictly between the 0.4 and 0.8
# thresholds, so every cell takes the ladder) and the spread noise sampled
# four times as fast and scaled by four, so neighbouring cells sit on
# different rungs. Then which four points rank nearest decides both the
# fluid level and whether a barrier separates two of them, and a point that
# should or should not be among them moves blocks. (A first version with a
# constant spread put every cell of a ladder band at one level, and the
# nearest-four set barely mattered: it scored identically with and without
# the wrap.) Lava below y 0 (Q5.8); sea_level -2040 keeps the global picker
# off lava at the bottom (the amendment's own condition for the bottom cell
# to be consulted).
#
# Read by tests/conformance/vanilla_aquifer_ywrap_test.cpp. Nothing this
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
spec="${work}/aqywrap_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

# A rule that changes nothing: it places stone, the default block, and only
# ever runs on the default block. (An anchor has to lie inside the dimension,
# [-2032, 2031] here, or the server refuses the pack.)
NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2031},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}
FAST_SPREAD = {"type": "minecraft:mul", "argument1": 4.0,
               "argument2": {"type": "minecraft:noise",
                             "noise": "minecraft:aquifer_fluid_level_spread",
                             "xz_scale": 4.0,
                             "y_scale": 0.7142857142857143 * 4.0}}

spec = [{"name": "yw", "min_y": -2032, "height": 4064,
         "legacy_random_source": False,
         "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
         "aquifers_enabled": True, "ore_veins_enabled": False, "sea_level": -2040,
         "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
         "surface_rule": NO_SURFACE,
         "router": {"barrier": REAL_BARRIER,
                    "fluid_level_floodedness": 0.6,
                    "fluid_level_spread": FAST_SPREAD,
                    "lava": -1.0,
                    "preliminary_surface_level": 2200.0}}]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
