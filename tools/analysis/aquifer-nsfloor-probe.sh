#!/usr/bin/env bash
# Stratum — the aborting near-surface floor: lambda, or the dry sentinel?
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-nsfloor-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `cellFluidLevel` floors an aborting near-surface cell at
# `lambda` (aquifer_lattice.cpp), while the two other dry outcomes return the
# sentinel `kNeverLevel`. No block readout can tell the two apart: a cell at
# either level is dry at every y >= lambda, and below lambda the global lava
# sea takes the block before the lattice is consulted. They part only in the
# barrier's pressure term, which weighs a source's level whether or not the
# source is readable — and every world that reaches this branch so far
# (aquifer-nearsurface-probe.sh, aquifer-psl-probe.sh, aquifer-lowsea-probe.sh)
# holds `barrier` at -2.0, where no barrier can form.
#
# So this is the near-surface probe's field with the barrier switched on:
# `preliminary_surface_level` is the same three-armed `range_choice` (HIGH
# above the ocean gate, MID below it, LOW below the scan's abort threshold),
# which drives cells into the aborting floor next to cells that flood to the
# sea; `barrier` is vanilla's own noise; floodedness is a constant 0.9, past
# both of the level rule's gates, so the floor branch and the sea are the only
# outcomes in play. Each scale ships a readout dimension, exactly as
# aquifer-psl-probe.sh's do, so a consumer reads the field out of a region
# file rather than reconstructing it.
#
# `lava` is 0.0, so every fluid is water and the fluid type plays no part.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so a second seed lands beside the first instead of on it.
#
# Read by tests/conformance/vanilla_aquifer_nsfloor_test.cpp. Nothing this
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

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/nsfloor_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

HIGH, MID, LOW = 96.0, -20.0, -70.0
SEA = 63
T_LOW, T_HIGH = -0.25, 0.25
MIN_Y = -64
HEIGHT = 384

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}

def field(scale, low, mid, high):
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": scale, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": T_LOW,
            "when_in_range": low,
            "when_out_of_range": {
                "type": "minecraft:range_choice", "input": noise,
                "min_inclusive": -1000.0, "max_exclusive": T_HIGH,
                "when_in_range": mid, "when_out_of_range": high}}

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, scale):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": REAL_BARRIER, "lava": 0.0,
                       "preliminary_surface_level": field(scale, LOW, MID, HIGH),
                       "fluid_level_floodedness": 0.9,
                       "fluid_level_spread": 0.0}}

spec = []
for tag, scale in (("8", 1.0), ("16", 0.5)):
    spec.append(aquifer("nsb_" + tag, scale))
    # The readout: same noise, same thresholds, an indicator instead of a
    # surface, so terrain height names the arm per column.
    spec.append({"name": "nsr_" + tag, "function": field(scale, -1.0, 0.0, 1.0)})

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
