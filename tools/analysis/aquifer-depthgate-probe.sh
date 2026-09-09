#!/usr/bin/env bash
# Stratum — which surface reading gates the aquifer's depth path.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-depthgate-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `cellFluidLevel`'s near-surface path gates on `gate`, the
# scan's prefix minimum — settled, and used the same way in three other
# places. The depth path gates on a DIFFERENT field, `anchor`, the single
# sample at the cell's own quart position. That asymmetry is single-sourced
# (SPEC §10, MA blocker 1): one agent, one instrument family, and it is
# exactly the shape — one path reading the minimum, a neighbouring path
# reading a single sample — that has been wrong twice before in this codebase
# (the near-surface abort exemption, and the aborting-floor's comparand).
#
# THE FIELD. Two large regions, HIGH (100) and LOW (40), both comfortably
# above the abort threshold — nothing here aborts, which keeps this probe
# about the GATING question alone, not entangled with abort at all. The
# region size (~100 blocks) is deliberately larger than the scan window's own
# reach (48 blocks west, 16 the other three directions), so near every region
# boundary there are cells whose ANCHOR sample lands in the HIGH region — its
# own quart position sits outside the LOW patch — while at least one WINDOW
# OFFSET reaches into the LOW patch, pulling `gate` down to 40 while `anchor`
# stays at 100. That is the exact configuration blocker 1 asks for: the two
# differ by 60, eight times the required margin.
#
# A companion readout dimension observes the field directly rather than
# reconstructing it — the same technique `aquifer-psl-probe.sh` established.
#
# THE DISCRIMINATION. With `anchor` HIGH and `gate` LOW at such a cell, the
# ocean gate at `sea_level` 63 sits at 55. `gate`(40) < 55 while `anchor`(100)
# is not — so if the depth path gates on `gate` (the hypothesis), it fires and
# adds a reach-dependent bonus to `floodedness`; if it gates on `anchor`
# (current code), it does not, and the cell falls through to the
# depth-independent branches, comparing raw `floodedness` against the fixed
# 0.4 / 0.8 thresholds with no bonus at all. The two hypotheses predict
# DIFFERENT LEVELS — not a shifted threshold, a different branch of the
# function entirely — so a handful of floodedness values, none of them
# exotic, already separate them; nine dimensions comb the range 0.05 to 0.85
# to catch whatever reach values the cells in this probe actually produce,
# and each one lets the analysis compute both hypotheses' predictions
# directly from the cell's own measured `anchor`, `gate` and `centreY` rather
# than from a single hand-picked case.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
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
spec="${work}/depthgate.json"

python3 - "${spec}" <<'PY'
import json, sys

SEA = 63
HIGH, LOW = 100.0, 40.0
# ~100-block patches: comfortably larger than the scan window's 48-block
# reach, so an anchor sitting outside a LOW patch still has a window offset
# that reaches into one.
XZ_SCALE = 0.08
THRESHOLD = 0.0  # noise sign alone divides HIGH from LOW

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def psl_field():
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": XZ_SCALE, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": THRESHOLD,
            "when_in_range": LOW, "when_out_of_range": HIGH}

def aquifer(name, floodedness):
    return {"name": name,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": 0.0,
                       "preliminary_surface_level": psl_field(),
                       "fluid_level_floodedness": floodedness,
                       "fluid_level_spread": 0.0}}

spec = []
for flood in (0.05, 0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85):
    tag = "f" + str(int(round(flood * 100)))
    spec.append(aquifer(tag, flood))

# The readout: same noise, same threshold, an indicator instead of a surface.
noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
         "xz_scale": XZ_SCALE, "y_scale": 0.0}
spec.append({"name": "r", "function":
             {"type": "minecraft:range_choice", "input": noise,
              "min_inclusive": -1000.0, "max_exclusive": THRESHOLD,
              "when_in_range": -1.0, "when_out_of_range": 1.0}})

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
