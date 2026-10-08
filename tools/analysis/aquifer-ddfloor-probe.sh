#!/usr/bin/env bash
# Stratum — the deep-dark override over an aborted scan: the sentinel, or lambda?
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-ddfloor-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `cellLevel` returns the deep-dark override's dry sentinel
# BEFORE an aborted scan's floor is reached, so an aborted cell under the
# override reads `kNeverLevel`, where the same cell outside it reads lambda
# (aquifer-nsfloor-probe.sh measured that floor). The documented order is the
# other one: spec Q5.3's short-circuits precede the level rule, and Q5.9
# forces only that rule's floodedness comparands, so an aborted deep-dark
# cell would take the submerged surface's status — lambda — like any other
# aborted cell. No other probe has an aborting scan under the override:
# every one that aborts holds `erosion` and `depth` at 0.
#
# So this is aquifer-nsfloor-probe.sh's field — the same three-armed
# `preliminary_surface_level` (HIGH above the ocean gate, MID below it, LOW
# below the scan's abort threshold), vanilla's barrier noise, a readout
# dimension per scale — with `erosion` -0.5 and `depth` 1.0, past both of
# Q5.9's thresholds everywhere. Floodedness is 0.9 and plays no part off the
# near-surface path (the override forces both comparands); on it, the
# near-surface return compares no floodedness at all. The two orders part
# only in the barrier's pressure term, between an aborted cell (sentinel or
# lambda) and a neighbour flooded to the sea.
#
# `lava` is 0.0, so every fluid is water and the fluid type plays no part.
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
spec="${work}/ddfloor_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

HIGH, MID, LOW = 96.0, -20.0, -70.0
SEA = 63
T_LOW, T_HIGH = -0.25, 0.25
MIN_Y = -64
HEIGHT = 384
# Past both of Q5.9's thresholds (erosion < -0.225, depth > 0.9) by a margin.
DEEP_DARK = {"erosion": -0.5, "depth": 1.0}

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
    router = {"barrier": REAL_BARRIER, "lava": 0.0,
              "preliminary_surface_level": field(scale, LOW, MID, HIGH),
              "fluid_level_floodedness": 0.9,
              "fluid_level_spread": 0.0}
    router.update(DEEP_DARK)
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE, "router": router}

spec = []
for tag, scale in (("8", 1.0), ("16", 0.5)):
    spec.append(aquifer("ddk_" + tag, scale))
    # The readout: same noise, same thresholds, an indicator instead of a
    # surface, so terrain height names the arm per column.
    spec.append({"name": "ddr_" + tag, "function": field(scale, -1.0, 0.0, 1.0)})

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

# A frozen world keeps every fluid tick it schedules, and these worlds are
# water from wall to wall: density-probe.sh's 6 GB default is not enough for
# their siblings in aquifer-nsfloor-probe.sh. Overridable, as there.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
