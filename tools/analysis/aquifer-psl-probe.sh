#!/usr/bin/env bash
# Stratum — an aquifer probe whose preliminary_surface_level VARIES in space.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-psl-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS, and it is the most important probe in the aquifer track.
#
# This project has built about 1370 aquifer probe dimensions and every one of
# them held `preliminary_surface_level` at a CONSTANT. Under a constant surface
# all four of the level rule's surface consumers — the near-surface gate, the
# depth, the ladder's cap and the depth path's own gate — read the same number
# by construction, so a model that confuses them scores 1.00000 and says
# nothing. That is how this project arrived at a confident wrong law twice, and
# SPEC §10 makes a spatially varying surface a precondition for the aquifer
# milestone closing at all.
#
# THE FIELD IS THREE-VALUED, and three is the smallest number that works. On a
# two-valued field an aborting prefix-minimum is identically a point read, so
# the two models are indistinguishable; a third arm separates them at once.
# The arms are chosen for what each one does to the rule rather than for
# spread:
#
#   HIGH = 96   above `sea_level - 8`, so it never opens the near-surface path
#   MID  = -20  below that gate and above the scan's abort, so it opens it
#   LOW  = -70  below the abort threshold of -62, so it aborts the scan
#
# THE FIELD IS ALSO OBSERVABLE. Each scale ships a READOUT dimension whose
# terrain height encodes which arm each column is in — the same nested
# `range_choice` over the same noise at the same thresholds, emitting -1, 0
# and +1 instead of the three surface values. So a consumer reads the field
# out of a region file rather than reconstructing it, and a mistake in the
# reconstruction cannot be mistaken for a disagreement with the server.
#
# Six aquifer dimensions: three feature sizes (about 8, 16 and 32 blocks, the
# 16 chosen to match the scan window's own pitch) crossed with two
# floodedness values. 0.5 sits between the two gates so the ladder is in play;
# 0.0 sits below both, so only the ocean branch's reach can flood a cell,
# which is what puts the reach's own surface consumer under test. Three
# constant-surface controls at each arm value complete it: those must
# reproduce what 1370 earlier dimensions already showed, and a harness that
# fails them is measuring itself.
#
# `lava` is pinned at 0.0 rather than -1.0 so that every aquifer fluid is
# water and the readout is a clean level rather than a level and a type. That
# is deliberately the same trap this probe exists to escape, in a variable
# this probe is not about; it is stated here so the next campaign knows.
#
# Nothing it writes is committed: worlds are Mojang-derived (SPEC §12).
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
spec="${work}/pslvar.json"

python3 - "${spec}" <<'PY'
import json, sys

HIGH, MID, LOW = 96.0, -20.0, -70.0
SEA = 63
# Thresholds on stratum:probe_noise, chosen to give the three arms comparable
# area. They are read back from the readout dimension rather than trusted.
T_LOW, T_HIGH = -0.25, 0.25

def field(scale, low, mid, high):
    """low below T_LOW, mid between, high above — nested range_choice."""
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": scale, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": T_LOW,
            "when_in_range": low,
            "when_out_of_range": {
                "type": "minecraft:range_choice", "input": noise,
                "min_inclusive": -1000.0, "max_exclusive": T_HIGH,
                "when_in_range": mid, "when_out_of_range": high}}

# Never fires: the aquifer's own blocks are the only ones in these worlds.
NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, psl, floodedness):
    return {"name": name,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": floodedness,
                       "fluid_level_spread": 0.0}}

spec = []
for tag, scale in (("8", 1.0), ("16", 0.5), ("32", 0.25)):
    for suffix, flood in (("f5", 0.5), ("f0", 0.0)):
        spec.append(aquifer("v" + tag + suffix, field(scale, LOW, MID, HIGH), flood))
    # The readout: same noise, same thresholds, an indicator instead of a
    # surface. Terrain height then names the arm, per column, from the
    # server's own arithmetic.
    spec.append({"name": "r" + tag, "function": field(scale, -1.0, 0.0, 1.0)})

for name, value in (("c96", HIGH), ("cm20", MID), ("cm70", LOW)):
    spec.append(aquifer(name, value, 0.5))

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
