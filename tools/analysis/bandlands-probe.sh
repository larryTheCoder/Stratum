#!/usr/bin/env bash
# Stratum — bandlands, isolated: the whole column solid, and bandlands alone
# deciding what replaces it.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/bandlands-probe.sh --accept-eula [seed ...]
#
# WHY THIS EXISTS. `bandlands` is the last unrunnable construct in vanilla's
# overworld tree (SPEC §11): it paints terracotta banding, and an earlier,
# unsaved probe found the colour FREQUENCIES over one column — plain
# terracotta, orange, red, white, light grey, in that order — but not the rule
# that decides which colour goes where. In vanilla's own tree it sits under a
# `biome` condition and several depth/height gates, reachable only inside a
# badlands biome at the right depth; probing it there means finding a rare
# biome AND threading every gate above it correctly first. Put directly at the
# root of a probe's own tree instead — exactly how `temperature`, `hole`,
# `steep` and `above_preliminary_surface` were each measured — and it is asked
# at EVERY block of a fully solid column, unconditionally.
#
# THE SHAPE. `density-probe.sh`'s "surface rule" form: `raw_final_density` is
# a bare positive constant, so `default_block` (stone) fills the whole column
# from min_y to the top; `surface_rule` is `{"type":"minecraft:bandlands"}`
# alone. Anywhere it still reads "stone" back, bandlands declined to fire
# there; everywhere else names which of the five colours it chose. One
# dimension per seed, since `density-probe.sh` fixes the world seed for the
# whole server and bandlands' colour table is seeded from it (SPEC §11).
#
# 128x128 columns (the script's own CHUNKS=8) over the full -64..319 height —
# about 6.3M blocks a seed — so a pattern that depends on x, z AND y all
# has room to show itself, not just the one column the earlier probe read.
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
seeds=("${args[@]}")
[[ ${#seeds[@]} -gt 0 ]] || seeds=(42)

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/bandlands.json"

python3 - "${spec}" <<'PY'
import json, sys
json.dump([{
    "name": "band",
    "raw_final_density": 1.0,
    "surface_rule": {"type": "minecraft:bandlands"},
}], open(sys.argv[1], 'w'))
PY

for seed in "${seeds[@]}"; do
    echo "==> seed ${seed}" >&2
    tools/analysis/density-probe.sh \
        $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
        --spec "${spec}" --seed "${seed}"
    probe_dir="${repo_root}/.fixtures/1.21.11/probes/bandlands"
    dst="${probe_dir}/band-seed-${seed//-/m}"
    mkdir -p "${dst}"
    mv "${probe_dir}/band/r.0.0.mca" "${dst}/r.0.0.mca"
    cp "${probe_dir}/manifest.json" "${dst}/manifest.json"
    echo "==> wrote ${dst}/r.0.0.mca" >&2
done
