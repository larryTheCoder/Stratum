#!/usr/bin/env bash
# Stratum — the open question: how a NAMED noise's identifier becomes a seed
# under `legacy_random_source`.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/legacy-seed-probe.sh --accept-eula [seed]
#
# THIS PROBE ANSWERS NOTHING BY ITSELF. It generates the worlds that
# tools/analysis/legacy-seed-analyze.cpp scores candidate derivations against,
# and its value is that the worlds can be regenerated at all: the earlier
# investigation ran against a locally edited density-probe.sh that hardcoded
# `legacy_random_source: False`, so not one of its measurements could be
# reproduced from the repository. Everything a spec names now lives in the
# spec or in its sidecar.
#
# THE DIMENSIONS, each a single `noise` node read through density-probe.sh's
# flat_cache + gradient inversion, and each paired with a modern-source mirror
# where one exists:
#
#   leg_single / mod_single   stratum:na    one octave
#   leg_twin                  stratum:nb    byte-identical parameters to na
#                                           under a different identifier, so
#                                           that "does the name participate at
#                                           all" is a comparison and not an
#                                           argument
#   leg_multi  / mod_multi    stratum:nmulti three octaves, all present
#   leg_skip   / mod_skip     stratum:nskip  three octave SLOTS with the
#                                           middle amplitude zero
#
# WHY nskip IS HERE, and it is not for the octave-skipping rule. A noise with
# an interior zero amplitude has a narrower spread of values than a dense one
# at the same octave count, and the readback's agreement criterion is a fixed
# band in VALUE space — so a wrong candidate agrees with such a dimension far
# more often than with a dense one, for no reason but the geometry. An earlier
# write-up of this investigation quoted one "empirical null" across
# dimensions, and a 16.4% score on the skip-shaped dimension read as an
# outlier against 0-5% siblings. It is not an outlier; it is that dimension's
# own null, and this probe exists partly so the analyzer can measure that
# rather than assume it. See SPEC §11.
#
# ONE OUTPUT SCALE, the fine one, because the null also moves with the
# quantum and holding it fixed across dimensions is the only way the
# per-dimension comparison above means anything.
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
spec="${work}/legseed_s${seed}.json"

# The sidecar density-probe.sh reads: the noises this spec names, shipped with
# it. A spec that references a noise it does not define is a spec nobody can
# re-run.
cat > "${work}/legseed_s${seed}.noises.json" <<'NOISES'
{
  "stratum:na":     {"firstOctave": -3, "amplitudes": [1.0]},
  "stratum:nb":     {"firstOctave": -3, "amplitudes": [1.0]},
  "stratum:nmulti": {"firstOctave": -5, "amplitudes": [1.0, 1.0, 1.0]},
  "stratum:nskip":  {"firstOctave": -5, "amplitudes": [1.0, 0.0, 1.0]}
}
NOISES

python3 - "${spec}" <<'PY'
import json, sys

FINE = 2.0


def dimension(name, legacy, noise, scale=FINE):
    return {"name": name,
            "legacy_random_source": legacy,
            "function": {"type": "minecraft:mul",
                         "argument1": scale,
                         "argument2": {"type": "minecraft:noise",
                                       "noise": noise,
                                       "xz_scale": 1.0,
                                       "y_scale": 0.0}}}


spec = [
    dimension("leg_single", True, "stratum:na"),
    dimension("mod_single", False, "stratum:na"),
    dimension("leg_twin", True, "stratum:nb"),
    dimension("leg_multi", True, "stratum:nmulti"),
    dimension("mod_multi", False, "stratum:nmulti"),
    dimension("leg_skip", True, "stratum:nskip"),
    dimension("mod_skip", False, "stratum:nskip"),
    # The same noise as leg_skip, read at a quarter and at a sixteenth of the
    # output scale. Nothing about the SEEDING changes; only the width of the
    # band a wrong candidate has to land in. These two are what turn "the
    # empirical null" from an assumed constant into a measured function of the
    # readback — see the analyzer's header and SPEC §11.
    dimension("leg_skip_q4", True, "stratum:nskip", FINE / 4.0),
    dimension("leg_skip_q16", True, "stratum:nskip", FINE / 16.0),
]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
