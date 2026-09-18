#!/usr/bin/env bash
# Stratum — WHERE the ore-vein system is allowed to put a block, and what may
# later paint over it.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/ore-vein-placement-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS, and why ore-vein-probe.sh could not answer it. That probe
# makes the column FULLY SOLID and runs NO surface rule, on purpose — which is
# exactly what makes it a clean read on the three random draws, and exactly
# what makes it blind to both questions below. Once the derivation was
# confirmed per block on 79790 candidates there, the only things left between
# "confirmed derivation" and "correct placement" were these two, and guessing
# either would have been the plausible-but-wrong class SPEC §8 exists to keep
# out.
#
# THREE DIMENSIONS, all otherwise identical to ore-vein-probe.sh's — the real
# vein router entries, aquifers on, veins on — so the CANDIDATE SET is the
# same as that probe's at the same seed (the vein router does not read the
# density, and nothing here changes the router).
#
#   lowcut / highcut — `raw_final_density` is a `y_clamped_gradient` running
#     +1 -> -1 that crosses zero INSIDE a vein range: at y = -34 for lowcut
#     (iron's [-60, -8]) and y = +25 for highcut (copper's [0, 50]). The same
#     positions therefore appear both over solid ground and over air.
#
#     MEASURED at seed 100: of the candidates the server left as air, the
#     confirmed chain would have placed 17813 vein blocks and the server
#     placed ZERO, over 25509 air positions. Of the candidates it left solid,
#     all 11455 came back EXACT — including the ones the aquifer's own barrier
#     turned solid against a negative density, which a fully solid probe
#     cannot produce at all. So veins replace solid ground only, and "solid"
#     means whatever the filler ended up placing, not merely
#     `final_density > 0`.
#
#   repaint — solid throughout like ore-vein-probe.sh, but with an
#     UNCONDITIONAL surface rule painting `minecraft:diamond_block`. Every
#     block the surface system is allowed to touch becomes a block that is
#     neither stone nor any vein block, so each candidate position answers
#     plainly: its vein block means the surface system left it alone,
#     diamond_block means it painted over it.
#
#     MEASURED at seed 100: all 12934 positions the chain calls a vein block
#     came back as that vein block, and all 5548 candidate positions it calls
#     stone came back as diamond_block. So surface rules repaint the default
#     block and NOT a vein block. This one is load-bearing rather than
#     academic: the overworld's own `deepslate` rule is unconditionally true
#     below y = -8, and iron's whole range is [-60, -8], so a filler that let
#     surface rules win would erase every iron vein in the world while every
#     existing golden test — all of which run with ore veins off — stayed
#     green.
#
# Nothing this writes is committed: worlds and their regions are Mojang-derived
# (SPEC §12).
set -euo pipefail

accept_eula=0
seed=100
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        -h|--help) sed -n '2,55p' "$0"; exit 0 ;;
        *) seed="$1"; shift ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/oreveinplacement.json"

settings=".fixtures/1.21.11/worldgen/noise_settings/overworld.json"
[[ -f "${settings}" ]] || { echo "error: no ${settings}; run tools/fetch-vanilla first" >&2; exit 1; }

env SETTINGS="${settings}" python3 - "${spec}" <<'PY'
import json, os, sys

MIN_Y = -64
HEIGHT = 384
SEA = 63

router = json.load(open(os.environ['SETTINGS']))['noise_router']

# Never fires — same as ore-vein-probe.sh, and for the same reason.
NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

# Always fires, with a block that is neither stone nor any vein block.
REPAINT = {"type": "minecraft:block",
           "result_state": {"Name": "minecraft:diamond_block"}}


def dimension(name, density, surface_rule):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": density,
            "aquifers_enabled": True, "ore_veins_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:air"},
            "size_horizontal": 1, "size_vertical": 2, "surface_rule": surface_rule,
            "router": {"vein_toggle": router['vein_toggle'],
                       "vein_ridged": router['vein_ridged'],
                       "vein_gap": router['vein_gap']}}


def crossing(from_y, to_y):
    # +1 at from_y falling to -1 at to_y: solid below the midpoint, air above.
    return {"type": "minecraft:y_clamped_gradient", "from_y": from_y, "to_y": to_y,
            "from_value": 1.0, "to_value": -1.0}


SOLID = {"type": "minecraft:constant", "argument": 1.0}

json.dump([dimension("lowcut", crossing(-64, -4), NO_SURFACE),
           dimension("highcut", crossing(-5, 55), NO_SURFACE),
           dimension("repaint", SOLID, REPAINT)],
          open(sys.argv[1], "w"))
print(3, "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
