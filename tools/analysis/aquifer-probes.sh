#!/usr/bin/env bash
# Stratum — every aquifer probe corpus the conformance cases read, in one go.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-probes.sh --accept-eula
#
# Each corpus has its own generator under tools/analysis, and each generator
# says why its world looks the way it does. This only runs them, one server at
# a time, with the seeds the cases expect — so that "regenerate the aquifer
# probes" is one command rather than a list someone has to keep in their head.
# Every corpus comes out frozen (density-probe.sh, SPEC §7) and records its
# seed, which the cases check before scoring it.
#
# It takes a while: about fifteen server starts, one to three minutes each.
# A failed generator stops the run, naming it. Nothing it writes is committed:
# worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) echo "unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done
[[ ${accept_eula} -eq 1 ]] || {
    echo "error: this runs the Minecraft server, so it needs --accept-eula" >&2
    exit 1
}

run() {
    printf '==> %s\n' "$*" >&2
    "$@" --accept-eula || { printf 'error: %s failed\n' "$*" >&2; exit 1; }
}

run tools/analysis/aquifer-psl-probe.sh 42
run tools/analysis/aquifer-barrier-probe.sh 42
for seed in 42 31337 8675309; do
    run tools/analysis/aquifer-deepfloor-probe.sh "${seed}"
    run tools/analysis/aquifer-waterlava-probe.sh "${seed}"
    run tools/analysis/aquifer-nsfloor-probe.sh "${seed}"
    run tools/analysis/aquifer-ddfloor-probe.sh "${seed}"
done
run tools/analysis/aquifer-depthgate-probe.sh 42
run tools/analysis/aquifer-fluidtype-probe.sh 42
run tools/analysis/aquifer-lowsea-probe.sh 42
run tools/analysis/aquifer-nearsurface-probe.sh 42
run tools/analysis/aquifer-lavarun-probe.sh 42
for seed in 42 31337; do
    run tools/analysis/aquifer-capfloor-probe.sh "${seed}"
done
# One server start per seed, all four seeds the comb cases read.
run tools/analysis/aquifer-comb-probe.sh
run tools/analysis/aquifer-on-probe.sh -1
run tools/analysis/aquifer-free-probe.sh -1

printf '==> every aquifer probe corpus regenerated; now: ctest --preset conformance\n' >&2
