#!/usr/bin/env bash
# Stratum — every aquifer probe corpus the conformance cases read, in one go.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-probes.sh --accept-eula
#
# Each corpus has its own generator under tools/analysis, and each generator
# says why its world looks the way it does. Which ones, with the seeds the
# cases expect, is tools/probe-worlds' table, the one CI runs (SPEC §7); this
# is that command for every unit whose generator is an aquifer-*-probe.sh, so
# a new aquifer corpus is added there, once. Twenty-eight server starts, one
# at a time, about seventy minutes on a 4-core machine, nearly half of it the
# two capfloor seeds. Each corpus is replaced whole and comes out frozen
# (density-probe.sh, SPEC §7), recording its seed, which the cases check. A
# failed generator is named, its corpora are left as they were before it
# ran, and the rest still run; then every corpus is checked
# (tools/probe-worlds verify).
#
# probes/nearsurface is not among them: no conformance case reads it. For
# aquifer-nearsurface-analyze, run tools/analysis/aquifer-nearsurface-probe.sh
# --accept-eula 42. Nothing this writes is committed or uploaded: worlds are
# Mojang-derived (SPEC §12).
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "${repo_root}/tools/probe-worlds" generate --only 'aquifer-*' "$@"
