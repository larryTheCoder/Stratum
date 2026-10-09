#!/usr/bin/env bash
# Stratum — the aquifer's cell centres where the position mix's x product
# overflows: past cell index 686, on both signs.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-farcell-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. The aquifer draws each cell's centre from the position mix
# (`rng::positionSeed`) at the CELL index, and the mix multiplies x as a
# 32-bit int: `(int)(x * 3129871)`. That product leaves the int range only
# past |x| 686, about 11 000 blocks from the origin, and every aquifer corpus
# this project had sat at cell indices 0..7 (the comb, the legacy pair, the
# level and barrier worlds). So "the aquifer's x term is a 32-bit product"
# rested on the surface depth's jitter, the same mix unsalted, measured at
# x 2176..6911 on the clamp probe (SPEC §11) — the same function, inferred
# rather than seen. This puts an aquifer where it is seen.
#
# Two worlds per seed, one server start each, because density-probe.sh
# forceloads one 8x8 block of chunks per start and a block has one sign:
#
#   farcell_pos_s<seed>  origin chunk (704, 704):    cells 704..711 on x and
#                        z, region r.22.22
#   farcell_neg_s<seed>  origin chunk (-712, -712):  cells -712..-705, region
#                        r.-23.-23
#
# Cells are 16 blocks wide and start on a chunk boundary, so the forceloaded
# chunks ARE the cells the readout scores; every one is past 686 on both
# axes. The z term is a 64-bit product, which a 32-bit one would leave past
# |z| 18, so these cells part that rival too.
#
# Each world holds the legacy aquifer probe's two aquifer arms
# (legacy-aquifer-probe.sh), unchanged: open void (`raw_final_density` -1),
# every router input a constant — barrier -2.0, fluid_level_floodedness 0.5,
# fluid_level_spread 0.0, lava -1.0, preliminary_surface_level 96 — water as
# default_fluid, sea_level 63, no surface rule that fires, a biome with no
# carvers or features:
#
#   mj  legacy_random_source false: Xoroshiro128++ centres
#   lj  legacy_random_source true:  java.util.Random centres, the same mix
#       XORed into the stream seed
#
# With every input a constant a block is the lattice's decision alone, so a
# case can replay each arm block for block through the call the filler
# makes, and read the comb probe's one-bit readout of each cell's vertical
# draw at y -42 (vanilla_aquifer_jitter_test.cpp) without a noise anywhere.
# No positive control (the legacy probe's lc/mc): lj against Xoroshiro
# centres is one, scored in the same case.
#
# Read by tests/conformance/vanilla_aquifer_farcell_test.cpp. Nothing this
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

# name : origin chunk x : origin chunk z. Each 8x8 block lies inside one
# region, which density-probe.sh requires and checks.
placements=(
    "pos:704:704"
    "neg:-712:-712"
)

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

eula=()
[[ ${accept_eula} -eq 1 ]] && eula+=(--accept-eula)

for placement in "${placements[@]}"; do
    IFS=':' read -r name origin_chunk_x origin_chunk_z <<< "${placement}"
    # The spec's NAME carries the placement and the seed (density-probe.sh
    # names its output after the spec), so neither overwrites the other.
    spec="${work}/farcell_${name}_s${seed}.json"
    python3 - "${spec}" <<'PY'
import json, sys

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}


def aquifer(name, legacy):
    return {"name": name, "min_y": -64, "height": 384,
            "legacy_random_source": legacy,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "ore_veins_enabled": False, "sea_level": 63,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0,
                       "fluid_level_floodedness": 0.5,
                       "fluid_level_spread": 0.0,
                       "lava": -1.0,
                       "preliminary_surface_level": 96.0}}


spec = [aquifer("mj", False), aquifer("lj", True)]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY
    echo "==> ${name}: seed ${seed}, origin chunk (${origin_chunk_x}, ${origin_chunk_z})" >&2
    tools/analysis/density-probe.sh "${eula[@]}" --spec "${spec}" --seed "${seed}" \
        --origin-chunk "${origin_chunk_x}" "${origin_chunk_z}"
done
