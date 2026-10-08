#!/usr/bin/env bash
# Stratum — check-determinism.sh rule 8 against workflows with known answers.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Rule 8 keeps CI's Mojang-derived files from being published as a workflow
# artifact (SPEC §12). A grep is easy to step around, so each spelling of an
# upload that is still valid YAML is tried here, one at a time, in a scratch
# tree holding only the lint script and what its other rules need.
# Plain bash 3.2: this runs on the macOS legs too.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

tree="${work}/repo"
mkdir -p "${tree}/tools/lint" "${tree}/cmake" "${tree}/tests" "${tree}/ext/tests" \
         "${tree}/.github/workflows"
cp "${repo_root}/tools/lint/check-determinism.sh" "${tree}/tools/lint/"
cp "${repo_root}/cmake/StratumDeterminism.cmake" "${tree}/cmake/"
lint="${tree}/tools/lint/check-determinism.sh"

# expect <exit code> <what> <pattern or -> — runs the lint on the tree as it is.
expect() {
    local want="$1" what="$2" pattern="$3"
    local got=0
    "${lint}" > "${work}/out" 2>&1 || got=$?
    if [[ "${got}" -ne "${want}" ]] || { [[ "${pattern}" != - ]] && ! grep -qF -- "${pattern}" "${work}/out"; }; then
        echo "FAIL: ${what}: exit ${got} (wanted ${want}), output:"; cat "${work}/out"; exit 1
    fi
    echo "ok: ${what}"
}

workflow="${tree}/.github/workflows/ci.yml"
printf 'jobs:\n  a:\n    steps:\n      - uses: actions/cache@v4\n' > "${workflow}"
printf '      # - uses: actions/upload-artifact@v4\n' >> "${workflow}"
expect 0 "a workflow that uploads nothing passes, comments included" "all determinism and provenance checks passed"

# Each spelling: <file under the tree> <its content>.
try() {
    local file="$1" text="$2" what="$3"
    mkdir -p "$(dirname "${tree}/${file}")"
    printf '%s\n' "${text}" > "${tree}/${file}"
    expect 1 "${what}" "${file}"
    rm "${tree}/${file}"
}
try .github/workflows/up.yml '      - uses: actions/upload-artifact@v4' "a step that uploads fails"
try .github/workflows/up.yml '      - uses: Actions/Upload-Artifact@v4' "in any case"
try .github/workflows/up.yml '      - "uses": actions/upload-artifact@v4' "with a quoted key"
try .github/workflows/up.yml "$(printf '      - uses: >-\n          actions/upload-artifact@v4')" \
    "with a folded value"
try .github/workflows/up.yml '      - uses: actions/upload-pages-artifact@v3' "a Pages upload fails"
try .github/actions/keep/action.yml '    - uses: actions/upload-artifact@v4' \
    "a local composite action under .github fails"
try ci/keep/action.yaml '    - uses: actions/upload-artifact@v4' \
    "a local composite action elsewhere fails"

echo "no-artifact-upload: all cases passed"
