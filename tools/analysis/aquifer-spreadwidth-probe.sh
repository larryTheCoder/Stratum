#!/usr/bin/env bash
# Stratum — the aquifer's spread quantization at 32-bit width: Q5.7's
# width amendment, where `3 * floor((10 * spread) / 3)` and the older
# `3 * floorDiv(floor(10 * spread), 3)` part.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-spreadwidth-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. IMPLEMENTER-RULING-01 (aquifer P2, the width amendments
# throughout Q1-Q8) asks that every division and halving be checked against
# spec/aquifer-spec.md's width amendments, and its §2 admits a claim to lib/
# only once a golden confirms it. Every amended floor and halving was already
# implemented as amended except one: Q5.7 says the quantized term is `3 * F`,
# F the 32-bit floor of `(10.0 * s) / 3.0`. This build floored `10 * s` to an
# int FIRST and grouped it into threes after. For any spread vanilla's routers
# make the two are the same integer; they part only where `10 * s` leaves the
# int range and `(10 * s) / 3` does not, which only a datapack reaches. So the
# spread here is pushed exactly there, through `mul` (a `constant` is bounded
# to +-1e6):
#
#   sc  spread 0.0                     control: the ladder, 40b + 20
#   sp  spread 1e6 * 429.49672995      10s = 4294967299.5, (10s)/3 =
#                                      1431655766.5: F = 1431655766, and
#                                      3F wraps to 2 -> the ladder at 40b + 22.
#                                      Floored first, 10s saturates and the
#                                      level wraps far below the world: dry.
#   sn  spread -1e6 * 429.49672995     F = -1431655767, 3F wraps to -5 -> the
#                                      ladder at 40b + 15. Floored first: dry.
#
# Everything else is the threshold probe's constant world (aquifer-threshold-
# probe.sh, arm tc): open void, barrier -2.0, floodedness 0.5, lava -1.0,
# surface 96, sea 63, water, no surface rule that fires, Xoroshiro. A block is
# the lattice's decision alone; below y 0 the ladder is lava (Q5.8).
#
# Read by tests/conformance/vanilla_aquifer_spreadwidth_test.cpp. Nothing this
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
spec="${work}/aqspread_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, math, sys

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

FACTOR = 429.49672995


def wrapped3(spread):
    """The amendment's quantized term: 3 * floor((10s)/3) at 32-bit width."""
    f = math.floor((10.0 * spread) / 3.0)
    assert -2**31 <= f < 2**31, f
    return ((3 * f + 2**31) % 2**32) - 2**31


for sign, want in ((1.0, 2), (-1.0, -5)):
    spread = (sign * 1000000.0) * FACTOR
    assert abs(10.0 * spread) > 2**31, spread
    assert wrapped3(spread) == want, (spread, wrapped3(spread))


def scaled(sign):
    return {"type": "minecraft:mul",
            "argument1": {"type": "minecraft:constant", "argument": sign * 1000000.0},
            "argument2": {"type": "minecraft:constant", "argument": FACTOR}}


def aquifer(name, spread):
    return {"name": name, "min_y": -64, "height": 384,
            "legacy_random_source": False,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "ore_veins_enabled": False, "sea_level": 63,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0,
                       "fluid_level_floodedness": 0.5,
                       "fluid_level_spread": spread,
                       "lava": -1.0,
                       "preliminary_surface_level": 96.0}}


spec = [
    aquifer("sc", 0.0),
    aquifer("sp", scaled(1.0)),
    aquifer("sn", scaled(-1.0)),
]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
