#!/usr/bin/env bash
# Stratum — ore veins under `legacy_random_source`, and over a default_block
# that is not stone, against their controls in the same world.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/legacy-orevein-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. No vanilla dimension combines `legacy_random_source` with
# `ore_veins_enabled`, so nothing on disk says how the vein source draws
# under the legacy source, and this build refused the pair by name (SPEC
# §11). The server generates the pair when a datapack asks, so this asks —
# the two probes it combines are legacy-aquifer-probe.sh (a legacy dimension
# and its flag-off control in one world) and ore-vein-probe.sh (a solid
# column with veins on). One world, one server start, one seed, and eight
# dimensions:
#
#   lv   veins on, legacy_random_source TRUE
#   mv   the identical dimension with the flag FALSE: the control, which
#        this build already replays block for block with the Xoroshiro
#        vein source
#   nv   mv with default_block minecraft:netherrack
#   dv   mv with default_block minecraft:deepslate[axis=x], a state that
#        spells out a property that is not the block's default (axis=y)
#   or   mv with vein_toggle a constant 0.59730469733476643 (copper only)
#   lr   lv with vein_toggle a constant 0.57947951108217244 (copper only)
#   la   lv with aquifers_enabled FALSE
#   ma   mv with aquifers_enabled FALSE: ore-vein-probe.sh's first world
#        read all stone with aquifers off, and this build had gated veins on
#        aquifers since; la and ma ask whether veins need them, under each
#        source. They do not: each is its aquifers-on twin block for block
#        (0 of 6291456 differing, at the four seeds tools/probe-worlds
#        generates; three legacy worlds, as 42's 48-bit twin is 42's). The
#        first world's stone was a seed confound — its seed, 42, has no
#        position in its window that the vein gate admits (SPEC §11, "Ore
#        veins under the legacy source")
#
# nv and dv ask the other open question: whether a vein replaces a
# default_block that is not stone, and with which blocks. The filler gates
# the vein on "the block is the dimension's default_block", so it predicts
# yes, with the same blocks; nothing had measured it.
#
# or and lr ask how the ORE ROLL compares its draw. Every draw is a 24-bit
# nextFloat, and a draw f and a chance c compare differently in float and
# in double only where (float) c rounds DOWN onto f itself. Each toggle was
# chosen from the RNG alone, before any world at the seed existed
# (tools/analysis/legacy-orevein-analyze.cpp `oreroll 42`), so that its
# chance sits a quarter of a float ulp above one member position's second
# draw at seed 42 — (58, 20, 0) under Xoroshiro, (20, 20, 0) under the
# legacy source. Everywhere else the two arms are ordinary readouts. The
# MEMBERSHIP roll's comparison is read off whichever positions' first draw
# is exactly 0.7F (`bucket`): 31337's (38, -31, 7) under the legacy source,
# and seed 2's (24, -45, 22) and (24, 45, 22) under Xoroshiro, which is why
# tools/probe-worlds generates seed 2 as well.
#
# THE WORLD. A fully solid column (`raw_final_density` +1), so the aquifer's
# own fluid decision never applies (Q2.2) and every block in both vein ranges
# is the default_block before the vein system looks at it. aquifers_enabled
# is TRUE in every arm but la and ma, so that those two are aquifers-off
# twins of lv and mv (it was first set true on the belief that veins place
# nothing without it, which la and ma refuted), and its five inputs are the
# constant 0, as everything else in the router is. The vein inputs are
# constants too, chosen so that EVERY block of both ranges is a candidate,
# which turns every block into a readout of the three draws:
#
#   vein_toggle   y_clamped_gradient -0.7 at y <= -1, +0.7 at y >= 0: iron's
#                 sign over its whole range [-60, -8], copper's over [0, 50],
#                 and |0.7| clears the richness threshold (0.6 at worst)
#                 everywhere; the ore chance is the clamp's top, 0.3
#   vein_ridged   -1.0 (< 0, so every candidate is rolled)
#   vein_gap      0.0 (> -0.3, so a won ore roll is an ore)
#
# Nothing in any dimension names a noise, the biome has no features and no
# carvers, and the surface rule never fires, so the vein system is the only
# consumer of the world's random source in any of them. Whatever differs
# between lv and mv, the flag did it through the vein source; lv differing
# from mv at all is also the positive control that the flag reached the world.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so each seed lands beside the others: probes/orevlegacy_s<seed>.
#
# Read by tests/conformance/vanilla_orevein_legacy_test.cpp and
# tools/analysis/legacy-orevein-analyze.cpp. Nothing this writes is
# committed: worlds are Mojang-derived (SPEC §12).
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
spec="${work}/orevlegacy_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

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


def veins(name, legacy, default_block):
    return {"name": name, "min_y": -64, "height": 384,
            "legacy_random_source": legacy,
            "raw_final_density": {"type": "minecraft:constant", "argument": 1.0},
            "aquifers_enabled": True, "ore_veins_enabled": True, "sea_level": 63,
            "default_block": default_block,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "surface_rule": NO_SURFACE,
            "router": {"vein_toggle": TOGGLE, "vein_ridged": -1.0, "vein_gap": 0.0}}


def ore_roll(name, legacy, toggle):
    arm = veins(name, legacy, STONE)
    arm["router"]["vein_toggle"] = toggle
    return arm


STONE = {"Name": "minecraft:stone"}
spec = [veins("lv", True, STONE),
        veins("mv", False, STONE),
        veins("nv", False, {"Name": "minecraft:netherrack"}),
        veins("dv", False, {"Name": "minecraft:deepslate", "Properties": {"axis": "x"}}),
        # Copper only, at an ore chance a quarter of a float ulp above one
        # position's second draw (see the header).
        ore_roll("or", False, 0.59730469733476643),
        ore_roll("lr", True, 0.57947951108217244),
        dict(veins("la", True, STONE), aquifers_enabled=False),
        dict(veins("ma", False, STONE), aquifers_enabled=False)]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

# Not `exec`: the EXIT trap above has to outlive the server run, or every run
# leaves its spec's work directory behind.
tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
