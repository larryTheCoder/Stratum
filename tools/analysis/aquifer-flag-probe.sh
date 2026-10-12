#!/usr/bin/env bash
# Stratum — `aquifers_enabled` false against true, over open ground below a
# raised sea, with ore veins on in both: the flag's own control.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-flag-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. density-probe.sh writes an entry's `aquifers_enabled`
# into its noise settings, and every aquifers-off arm a case read was a
# solid column or kept `sea_level` at the dimension's floor with an air
# default fluid — worlds the flag cannot change, so nothing showed it
# reaching the server (SPEC §11, "Ore veins under the legacy source": the
# legacy ore-vein probe's `la` and `ma` read as their aquifers-on twins,
# which is also what they would read if the flag never arrived). And veins
# with aquifers off had been measured over solid columns only. This builds
# the world where the flag must show: OPEN ground below a RAISED sea, so
# that with aquifers off the default fluid fills every open block below
# `sea_level` (and the global lava sea every one below min(-54, sea_level)),
# while with aquifers on the aquifer's lattice decides — air, water, lava or
# a barrier, cell by cell. One world, one server start, one seed, and four
# dimensions, two pairs that differ ONLY in `aquifers_enabled`:
#
#   mon   modern random source, aquifers ON, over CAVES — stratum:probe_noise
#         sampled in 3D (`xz_scale` = `y_scale` = 0.5), about half of every
#         layer open, its walls in every direction
#   moff  mon with aquifers OFF
#   lon   legacy_random_source TRUE, aquifers ON, over LAYERS: a noise-free
#         triangle wave in y (below), seven solid rows and nine open in every
#         sixteen from y -64 to 64, open above. A legacy dimension may name no
#         noise but the three climate ones (SPEC §11), so its terrain is y
#         alone and the lattice is all that varies across x and z
#   loff  lon with aquifers OFF
#
# EVERY OTHER INPUT IS A CONSTANT, the same in all four. sea_level 32, so
# the sea's top sits inside copper's vein range [0, 50] and vein blocks meet
# both the default fluid and the air above it with aquifers off; the default
# fluid is water (the aquifer refuses any other); the surface rule never
# fires. The aquifer's five inputs are the legacy aquifer probe's except the
# barrier — `barrier` 0.0, `fluid_level_floodedness` 0.5 (every cell on the
# ladder), `fluid_level_spread` 0.0, `lava` -1.0 (a level of -10 or below is
# lava), `preliminary_surface_level` 96 (so y_skip is 130 and the lattice is
# consulted over the whole vein range) — and its barrier sheets fall against
# a density that crosses zero everywhere, rather than the open void's -1.
# The vein inputs are the legacy ore-vein probe's, so every default_block
# position of both vein ranges is a candidate:
#
#   vein_toggle   y_clamped_gradient -0.7 at y <= -1, +0.7 at y >= 0
#   vein_ridged   -1.0 (< 0, so every candidate is rolled)
#   vein_gap      0.0 (> -0.3, so a won ore roll is an ore)
#
# so about 70% of the solid ground in y -60..-8 and 0..50 is a vein block,
# beside the fluid and air of the open ground next to it — and, with
# aquifers on, on the aquifer's own barriers too.
#
# What the pairs show: each arm is its own setting's fill block for block
# (the off arms the global picker everywhere, the on arms the lattice), and
# each pair differs by a large count, which is the positive control that
# `aquifers_enabled` reached the world through density-probe.sh.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so each seed lands beside the others: probes/aqflag_s<seed>.
#
# Read by tests/conformance/vanilla_aquifer_flag_test.cpp. Nothing this
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
spec="${work}/aqflag_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

SEA = 32

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

# -0.7 at and below y -1, +0.7 at and above y 0. Integer y never lands
# between the two, so the toggle takes exactly two values.
TOGGLE = {"type": "minecraft:y_clamped_gradient", "from_y": -1, "to_y": 0,
          "from_value": -0.7, "to_value": 0.7}

CAVES = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
         "xz_scale": 0.5, "y_scale": 0.5}


def fold(argument, at):
    """|argument - at|: one fold of the triangle wave."""
    return {"type": "minecraft:abs",
            "argument": {"type": "minecraft:add", "argument1": argument, "argument2": -at}}


# y itself from -64 to 64, held at 64 above. Four folds make a triangle
# wave of period 16 and height 8 out of it (|y| at 0, then at 32, 16 and 8),
# every value a whole number at a whole y; 0.25 * (3.5 - wave) is solid
# where the wave is 0..3 (seven rows of every sixteen) and open where it is
# 4..8, never within 0.125 of zero. Above y 64 the wave is 8: open, -1.125.
WAVE = fold(fold(fold(fold({"type": "minecraft:y_clamped_gradient",
                             "from_y": -64, "to_y": 64,
                             "from_value": -64.0, "to_value": 64.0}, 0.0),
                       32.0), 16.0), 8.0)
LAYERS = {"type": "minecraft:mul", "argument1": 0.25,
          "argument2": {"type": "minecraft:add", "argument1": 3.5,
                        "argument2": {"type": "minecraft:mul", "argument1": -1.0,
                                      "argument2": WAVE}}}


def arm(name, legacy, aquifers, density):
    return {"name": name, "min_y": -64, "height": 384,
            "legacy_random_source": legacy,
            "raw_final_density": density,
            "aquifers_enabled": aquifers, "ore_veins_enabled": True, "sea_level": SEA,
            "default_block": {"Name": "minecraft:stone"},
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "surface_rule": NO_SURFACE,
            "router": {"barrier": 0.0,
                       "fluid_level_floodedness": 0.5,
                       "fluid_level_spread": 0.0,
                       "lava": -1.0,
                       "preliminary_surface_level": 96.0,
                       "vein_toggle": TOGGLE, "vein_ridged": -1.0, "vein_gap": 0.0}}


spec = [arm("mon", False, True, CAVES),
        arm("moff", False, False, CAVES),
        arm("lon", True, True, LAYERS),
        arm("loff", True, False, LAYERS)]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

# Not `exec`: the EXIT trap above has to outlive the server run, or every run
# leaves its spec's work directory behind.
tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
