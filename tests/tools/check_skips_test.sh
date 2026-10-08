#!/usr/bin/env bash
# Stratum — tools/check-skips against ctest reports with known answers.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# The gate CI puts after the conformance suite. Every way it must fail is
# exercised here, because a gate that quietly passes looks exactly like one
# with nothing to report.
set -euo pipefail
command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping"; exit 77; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
check="${repo_root}/tools/check-skips"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# report <file> <name|status>...  — the shape `ctest --output-junit` writes.
report() {
    local file="$1"; shift
    {
        printf '<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="t" tests="%d">\n' "$#"
        local pair name status
        for pair in "$@"; do
            name="${pair%|*}"; status="${pair##*|}"
            printf '<testcase name="%s" classname="%s" time="0" status="%s">\n' "${name}" "${name}" "${status}"
            [[ "${status}" == notrun ]] && printf '<skipped message="SKIP_RETURN_CODE=4"/>\n'
            printf '<system-out>reason for %s</system-out>\n</testcase>\n' "${name}"
        done
        printf '</testsuite>\n'
    } > "${file}"
}

# expect <exit code> <what> <pattern or -> <command...>
expect() {
    local want="$1" what="$2" pattern="$3"; shift 3
    local got=0
    "$@" > "${work}/out" 2>&1 || got=$?
    if [[ "${got}" -ne "${want}" ]] || { [[ "${pattern}" != - ]] && ! grep -qF -- "${pattern}" "${work}/out"; }; then
        echo "FAIL: ${what}: exit ${got} (wanted ${want}), output:"; cat "${work}/out"; exit 1
    fi
    echo "ok: ${what}"
}

printf '# a comment\n\nregions conformance.golden one\nno-generator   conformance.orphan: with a colon, "quotes" & more\n' \
    > "${work}/list"
escaped='conformance.orphan: with a colon, &quot;quotes&quot; &amp; more'

report "${work}/ok.xml" 'conformance.a|run' 'conformance.b|fail' \
    'conformance.golden one|notrun' "${escaped}|notrun"
expect 0 "every skip listed, every listed case skipped" "2 did not; expected: 1 no-generator, 1 regions" \
    "${check}" "${work}/ok.xml" "${work}/list"

report "${work}/extra.xml" 'conformance.a|run' 'conformance.new|notrun' \
    'conformance.golden one|notrun' "${escaped}|notrun"
expect 1 "an unlisted skip fails, quoting its reason" "reason for conformance.new" \
    "${check}" "${work}/extra.xml" "${work}/list"

report "${work}/disabled.xml" 'conformance.a|disabled' \
    'conformance.golden one|notrun' "${escaped}|notrun"
expect 1 "an unlisted disabled test fails" "unexpected skip: conformance.a" \
    "${check}" "${work}/disabled.xml" "${work}/list"

report "${work}/stale.xml" 'conformance.golden one|run' "${escaped}|notrun"
expect 1 "a listed case that ran fails" "listed (regions) but ran: conformance.golden one" \
    "${check}" "${work}/stale.xml" "${work}/list"

report "${work}/gone.xml" 'conformance.golden one|notrun'
expect 1 "a listed case missing from the report fails" "but not in the report" \
    "${check}" "${work}/gone.xml" "${work}/list"

report "${work}/empty.xml"
expect 1 "an empty report fails" "holds no tests" "${check}" "${work}/empty.xml" "${work}/list"
expect 1 "a missing report fails" "cannot read the ctest report" \
    "${check}" "${work}/absent.xml" "${work}/list"
printf '<testsuite><testcase' > "${work}/broken.xml"
expect 1 "a truncated report fails" "cannot read the ctest report" \
    "${check}" "${work}/broken.xml" "${work}/list"

printf 'conformance.no reason given\n' > "${work}/noreason"
expect 1 "a line without a reason fails" "expected \"<reason> conformance.<test name>\"" \
    "${check}" "${work}/ok.xml" "${work}/noreason"
printf 'regions conformance.x\nregions conformance.x\n' > "${work}/twice"
expect 1 "a case listed twice fails" "listed twice" "${check}" "${work}/ok.xml" "${work}/twice"
expect 2 "usage" "usage:" "${check}" "${work}/ok.xml"

echo "check-skips: all cases passed"
