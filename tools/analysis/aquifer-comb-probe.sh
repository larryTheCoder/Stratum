#!/usr/bin/env bash
# Stratum — the open-void "comb" aquifer probes the jitter, selection and
# fluid-type conformance cases read.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-comb-probe.sh --accept-eula [seed ...]
#
# WHY THIS EXISTS. Three conformance cases —
# vanilla_aquifer_jitter_test.cpp, vanilla_aquifer_selection_test.cpp and
# vanilla_aquifer_fluid_type_test.cpp — read
# .fixtures/<version>/probes/comb_<seed>/{jv,elava}/r.0.0.mca for seeds 42,
# 7, 12345 and 999, and nothing in the tree could produce those worlds: the
# spec that made them predates this repository's history. This rebuilds it
# from what the three cases themselves assume, which is the only record of it
# there is — so a regenerated world that disagreed with them would fail them,
# not quietly pass.
#
# Both arms are open void — `raw_final_density` a constant -1, so no terrain
# exists and every solid block belongs to the aquifer — with aquifers on,
# `sea_level` 63, water as `default_fluid`, `preliminary_surface_level` a
# constant 96 and `fluid_level_floodedness` a constant 0.5 (every cell on the
# ladder, none dry), and the real `minecraft:aquifer_barrier` noise:
#
#   jv     `fluid_level_spread` 0 and `lava` -1.0, the constants every one of
#          this project's level probes used. The selection case's own level
#          model reads neither, and the jitter case reads fluid of either type.
#   elava  `fluid_level_spread` and `lava` on vanilla's own noises — the one
#          arm the fluid-type case scores, which evaluates both through the
#          overworld's own router and so needs them byte-identical to it.
#
# The three vanilla entries are copied from the fetched overworld's own
# `noise_router` at generation time rather than spelled here, for exactly that
# reason (tools/fetch-vanilla's output; read, never committed — SPEC §12).
#
# One world per seed; each run writes probes/comb_<seed>/{jv,elava}.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
seeds=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) seeds+=("${arg}") ;;
    esac
done
if [[ ${#seeds[@]} -eq 0 ]]; then
    seeds=(42 7 12345 999)
fi

version="${STRATUM_MINECRAFT_VERSION:-1.21.11}"
overworld=".fixtures/${version}/worldgen/noise_settings/overworld.json"
if [[ ! -f "${overworld}" ]]; then
    echo "no ${overworld}; run tools/fetch-vanilla --version ${version} first" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

for seed in "${seeds[@]}"; do
    spec="${work}/comb_${seed}.json"
    python3 - "${overworld}" "${spec}" <<'PY'
import json, sys

router = json.load(open(sys.argv[1]))["noise_router"]

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def arm(name, spread, lava):
    return {"name": name, "min_y": -64, "height": 384,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": 63,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "surface_rule": NO_SURFACE,
            "router": {"barrier": router["barrier"],
                       "fluid_level_floodedness": 0.5,
                       "fluid_level_spread": spread,
                       "lava": lava,
                       "preliminary_surface_level": 96.0}}

spec = [
    arm("jv", 0.0, -1.0),
    arm("elava", router["fluid_level_spread"], router["lava"]),
]
json.dump(spec, open(sys.argv[2], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY
    tools/analysis/density-probe.sh \
        $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
        --spec "${spec}" --seed "${seed}"
done
