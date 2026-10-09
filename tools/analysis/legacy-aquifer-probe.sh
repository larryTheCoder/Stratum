#!/usr/bin/env bash
# Stratum — the aquifer under `legacy_random_source`, against its flag-off
# control in the same world.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/legacy-aquifer-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. No vanilla dimension combines `legacy_random_source` with
# `aquifers_enabled`, so nothing on disk says where the aquifer's cell
# centres go under the legacy source, and this build refused the pair by
# name on a structural argument (SPEC §11). The server will generate the
# pair anyway when a datapack asks for it, so this asks: one world, one
# server start, one seed, and four dimensions.
#
#   lj   the open-void aquifer world, legacy_random_source TRUE
#   mj   the identical dimension with the flag FALSE: the control, which
#        this build already replays block for block
#   lc   stratum:probe_noise read through density-probe.sh's flat_cache
#        readout, legacy_random_source TRUE
#   mc   the same with the flag false
#
# lj and mj are the comb probe's `jv` arm (aquifer-comb-probe.sh) with ONE
# change, the barrier: jv reads the named noise minecraft:aquifer_barrier,
# whose own legacy seeding is a separate open question (SPEC §11) and would
# confound this one. Here every router input is a constant — barrier -2.0,
# fluid_level_floodedness 0.5 (every cell on the ladder), fluid_level_spread
# 0.0, lava -1.0 (so a level of -10 or below is lava), and
# preliminary_surface_level 96 — and the surface rule never fires. Nothing in
# either dimension names a noise, the biome has no features and no carvers,
# and so the only consumer of the world's random source in either dimension
# is the aquifer itself. Whatever differs between lj and mj, the flag did it
# through the aquifer.
#
# lc and mc are the POSITIVE control. A legacy dimension that came back
# identical to its control could mean "the flag does not reach the aquifer"
# or "the flag did not reach this world at all"; a named noise is known to
# move under the flag (legacy-seed-probe.sh, SPEC §11), so lc differing from
# mc in the same run is what lets lj == mj mean the first.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so each seed lands beside the others: probes/aqlegacy_s<seed>.
#
# Read by tests/conformance/vanilla_aquifer_legacy_test.cpp and
# tools/analysis/legacy-aquifer-analyze.cpp. Nothing this writes is
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
spec="${work}/aqlegacy_s${seed}.json"

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


def control(name, legacy):
    return {"name": name, "legacy_random_source": legacy,
            "function": {"type": "minecraft:mul", "argument1": 2.0,
                         "argument2": {"type": "minecraft:noise",
                                       "noise": "stratum:probe_noise",
                                       "xz_scale": 1.0, "y_scale": 0.0}}}


spec = [aquifer("lj", True), aquifer("mj", False), control("lc", True), control("mc", False)]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
