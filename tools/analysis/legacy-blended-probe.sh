#!/usr/bin/env bash
# Stratum — how a `legacy_random_source` dimension seeds `old_blended_noise`.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/legacy-blended-probe.sh --accept-eula [seed]
#
# THE QUESTION. `minecraft:old_blended_noise` carries no name, so it is the
# one noise a legacy dimension can seed without answering the open question
# next door (how a NAMED noise's identifier becomes an LCG seed — SPEC §11,
# still open). Three candidate rules were on the table:
#
#   modern      XoroshiroPositionalFactory(seed).fromHashOf("minecraft:terrain"),
#               which is what a flag-off dimension uses and this build's
#               BlendedNoise::modern
#   legacy      new java.util.Random(worldSeed) straight in, read the
#               pre-1.18 way — BlendedNoise::legacy
#   legacy+mod  the same Java LCG seeding, read the 1.21.11 way —
#               BlendedNoise::legacyFromWorldSeed
#
# `legacy` and `legacy+mod` share every draw and differ only in the reading,
# by a factor of exactly 128 at y = 0, so the probe separates them trivially
# once it separates either from `modern`.
#
# THE READOUT is density-probe.sh's: the dimension's whole final_density is
# `K * flat_cache(scale * old_blended_noise) + y_clamped_gradient(+1..-1)`,
# so each cell-corner column's terrain height inverts to a reading of the
# noise at (x, 0, z). See that script's header.
#
# WHAT MAKES IT A CONTROL. Each world carries the same two functions twice:
# once in a dimension declaring `legacy_random_source` and once in one that
# does not. If the rule under test were an artefact of the readback rather
# than of the seeding, it would score the same in both. It does not — the
# mirror is the point, and it is why `legacy_random_source` had to become a
# per-entry field of the spec before any of this was reproducible.
#
# TWO OUTPUT SCALES per dimension, because the inversion's resolution is
# 2 / height / (K * scale): the fine one (2.0) resolves the noise to 0.0074
# and the coarse one (0.5) to 0.030 but cannot clip, and a rule that only
# wins at one of them is a rule that won by quantisation.
#
# Read back by tools/analysis/legacy-blended-analyze.cpp; pinned by
# tests/conformance/vanilla_legacy_blended_test.cpp. Nothing it writes is
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

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
# The spec's basename is the fixture directory, so the seed goes in it: one
# world per seed, and three seeds is what separates "the rule" from "a
# coincidence at seed 42" (see the memory note this project keeps on
# single-seed coincidences).
spec="${work}/legblend_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

# Vanilla's own overworld numbers for this node, so the probe measures the
# node as it is actually configured rather than at parameters nothing uses.
BLENDED = {"type": "minecraft:old_blended_noise",
           "xz_scale": 0.25, "y_scale": 0.375,
           "xz_factor": 80.0, "y_factor": 60.0,
           "smear_scale_multiplier": 8.0}


def dimension(name, legacy, scale):
    return {"name": name,
            "legacy_random_source": legacy,
            "function": {"type": "minecraft:mul",
                         "argument1": scale,
                         "argument2": BLENDED}}


spec = [
    dimension("leg_fine", True, 2.0),
    dimension("leg_coarse", True, 0.5),
    dimension("mod_fine", False, 2.0),
    dimension("mod_coarse", False, 0.5),
]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
