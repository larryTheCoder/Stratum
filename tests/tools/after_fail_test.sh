#!/usr/bin/env bash
# Stratum — check-determinism.sh rule 9 against test sources with known answers.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Rule 9 keeps a statement from following an unconditional FAIL or SKIP,
# which MSVC reports as unreachable (C4702) and /WX turns into a build
# failure on the Windows legs alone. The rule is a line scan, so the
# spellings it must accept and the ones it must refuse are tried here, one
# at a time, in a scratch tree holding only the lint script and what its
# other rules need.
# Plain bash 3.2: this runs on the macOS legs too.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

tree="${work}/repo"
mkdir -p "${tree}/tools/lint" "${tree}/cmake" "${tree}/tests" "${tree}/ext/tests"
cp "${repo_root}/tools/lint/check-determinism.sh" "${tree}/tools/lint/"
cp "${repo_root}/cmake/StratumDeterminism.cmake" "${tree}/cmake/"
lint="${tree}/tools/lint/check-determinism.sh"

# try <exit code> <what> <source text> — lints a tree holding that one source.
try() {
    local want="$1" what="$2" text="$3"
    local file="${tree}/tests/case_test.cpp"
    printf '%s\n' "${text}" > "${file}"
    local got=0
    "${lint}" > "${work}/out" 2>&1 || got=$?
    rm "${file}"
    if [[ "${got}" -ne "${want}" ]] || { [[ "${want}" -ne 0 ]] && ! grep -qF -- "case_test.cpp" "${work}/out"; }; then
        echo "FAIL: ${what}: exit ${got} (wanted ${want}), output:"; cat "${work}/out"; exit 1
    fi
    echo "ok: ${what}"
}

try 0 "a FAIL that ends its block passes" \
'int f() {
    FAIL("no entry");
}'
try 0 "a FAIL over several lines, then the brace, passes" \
'int f() {
    FAIL("no entry "
         << name);
}'
try 0 "blank lines, comments and directives before the brace pass" \
'int f() {
    SKIP("no corpus"); // the probe is generated in CI

    // nothing else
#if 0
#endif
    }'
try 0 "FAIL_CHECK does not throw, so code after it passes" \
'int f() {
    FAIL_CHECK("no entry");
    return 0;
}'
try 1 "a return after FAIL fails" \
'int f() {
    FAIL("no entry");
    return {};
}'
try 1 "a return after SKIP fails" \
'int f() {
    SKIP("no corpus");
    return {};
}'
try 1 "a statement after a FAIL over several lines fails" \
'int f() {
    FAIL("no entry "
         << name); // why
    return {};
}'

echo "after-fail: all cases passed"
