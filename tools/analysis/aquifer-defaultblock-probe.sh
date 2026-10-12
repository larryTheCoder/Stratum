#!/usr/bin/env bash
# Stratum — which block the aquifer's barrier is, on a preset whose
# default_block is not stone (spec Q6.7).
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-defaultblock-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. Q6.7 says the aquifer never names a block: where it
# decides "solid" the caller writes the preset's own `default_block`, the
# block positive density writes. `ChunkFiller` does exactly that, but every
# world that could have checked it was blind to it. Every probe ran stone
# (density-probe.sh hard-coded it until this script), and the vanilla
# presets whose default_block is not stone (the Nether's netherrack, the
# End's end_stone) have aquifers off. A filler that wrote literal stone for
# the barrier, or dropped default_block's properties, passed every case.
#
# THE WORLD. One recipe, `aquifer-barrier-probe.sh`'s `d_neg1_0` exactly:
# vanilla's real barrier, floodedness and spread noises, `lava` 0.0, a
# constant `preliminary_surface_level` of 96, sea level 63, water, min_y -48
# (clear of the lava sea at min(-54, 63), so no lava is ever placed and
# nothing that flows can make or unmake a solid block), and a surface rule
# that never fires. Four dimensions of it, differing only in default_block
# and, for the last, in the density:
#
#   db_stone             D = -1, stone: the control, in the same world.
#   db_netherrack        D = -1, netherrack: no properties, and nothing in
#                        this world can make it but default_block.
#   db_deepslate_x       D = -1, deepslate[axis=x]: a property that is not
#                        the block's default (axis=y), so a barrier written
#                        as the bare block shows.
#   db_solid_deepslate_x D = +1, deepslate[axis=x]: solid everywhere, the
#                        positive-density side of the same question with
#                        aquifers on.
#
# Every solid block of the first three is the aquifer's barrier, and the
# readings differ on whole sets of positions: under Q6.7 each treatment's
# solids are its default_block at exactly the control's stone positions; a
# literal-stone barrier leaves them stone; a bare-block barrier writes
# deepslate[axis=y]. `vanilla_aquifer_default_block_test.cpp` reads it, and
# runs the shipped filler over the same spec.
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
[[ "${seed}" =~ ^-?[0-9]+$ ]] || { echo "error: the seed must be a whole number: ${seed}" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/defaultblock_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

SEA = 63
MIN_Y = -48
HEIGHT = 320

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}
REAL_FLOODEDNESS = {"type": "minecraft:noise",
                     "noise": "minecraft:aquifer_fluid_level_floodedness",
                     "xz_scale": 1.0, "y_scale": 0.67}
REAL_SPREAD = {"type": "minecraft:noise", "noise": "minecraft:aquifer_fluid_level_spread",
                "xz_scale": 1.0, "y_scale": 0.7142857142857143}

# Never fires: the server's surface pass writes over default_block, so a rule
# that did would repaint exactly the blocks this probe reads.
NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

STONE = {"Name": "minecraft:stone"}
NETHERRACK = {"Name": "minecraft:netherrack"}
DEEPSLATE_X = {"Name": "minecraft:deepslate", "Properties": {"axis": "x"}}

def aquifer(name, density, default_block):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": density},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_block": default_block,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": REAL_BARRIER, "lava": 0.0,
                       "preliminary_surface_level": 96.0,
                       "fluid_level_floodedness": REAL_FLOODEDNESS,
                       "fluid_level_spread": REAL_SPREAD}}

spec = [
    aquifer("db_stone", -1.0, STONE),
    aquifer("db_netherrack", -1.0, NETHERRACK),
    aquifer("db_deepslate_x", -1.0, DEEPSLATE_X),
    aquifer("db_solid_deepslate_x", 1.0, DEEPSLATE_X),
]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
