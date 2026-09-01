#!/usr/bin/env bash
# Stratum — repo policy checks that a compiler cannot make for us.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Enforces the parts of SPEC §5 / CLAUDE.md "Determinism rules" and SPEC §4
# layer separation that are textual facts about the repo:
#
#   1. no non-derived RNG (std::rand, mt19937, random_device, <random>)
#   2. no fast-math style float flags in any build configuration
#   3. lib/ never includes PHP or zend headers
#   4. no Mojang-derived artefacts sitting in the tree (SPEC §12)
#   5. the determinism flags themselves are still applied
#
# NOT enforced here, on purpose: "no raw % or >> on possibly-negative
# values". Detecting that textually produces false positives on streams and
# templates; it is covered by review, by the javamath helpers being the only
# sanctioned path, and by conformance goldens.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

failures=0
# Directories holding first-party code and build configuration.
scan_dirs=(lib cli ext tests cmake tools .github)
scan_files=(CMakeLists.txt CMakePresets.json)

fail() {
    printf 'DETERMINISM LINT FAILURE: %s\n' "$1" >&2
    failures=$((failures + 1))
}

# grep_first_party <pattern> [extra grep args...]
# Searches first-party sources only; build trees and vendored deps are out.
grep_first_party() {
    local pattern="$1"
    shift
    local targets=()
    local path
    for path in "${scan_dirs[@]}" "${scan_files[@]}"; do
        [[ -e "${path}" ]] && targets+=("${path}")
    done
    [[ ${#targets[@]} -eq 0 ]] && return 1
    grep -rnE "${pattern}" "${targets[@]}" \
        --exclude-dir=_deps \
        --exclude-dir=build \
        --exclude-dir=CMakeFiles \
        --exclude-dir=cmake-build-debug \
        --exclude-dir=cmake-build-release \
        "$@"
}

echo "== 1. non-derived RNG =="
# SPEC §5.3: only the project's Java LCG and Xoroshiro128++, seeded per
# (worldSeed, position, salt). Anything else is unreproducible by definition.
rng_pattern='\b(std::rand|srand|std::srand|mt19937(_64)?|minstd_rand|ranlux|knuth_b|random_device|default_random_engine|std::shuffle)\b|#include[[:space:]]*<random>'
if rng_hits="$(grep_first_party "${rng_pattern}" --exclude-dir=lint)"; then
    fail "non-derived RNG referenced (SPEC §5.3):"
    printf '%s\n' "${rng_hits}" >&2
else
    echo "  ok"
fi

echo "== 2. fast-math style float flags =="
# SPEC §5.4. cmake/StratumDeterminism.cmake is excluded: it *names* these
# flags in order to reject them.
float_pattern='(-ffast-math|-funsafe-math-optimizations|-fassociative-math|-freciprocal-math|-ffinite-math-only|-Ofast|/fp:fast)'
if float_hits="$(grep_first_party "${float_pattern}" \
        --exclude=StratumDeterminism.cmake --exclude-dir=lint)"; then
    fail "fast-math style flag in build configuration (SPEC §5.4):"
    printf '%s\n' "${float_hits}" >&2
else
    echo "  ok"
fi

echo "== 3. lib/ stays free of PHP/zend =="
# SPEC §4: lib/ is pure C++ so cli/ and the conformance harness can run the
# real engine without PocketMine. Marshaling lives in ext/ only.
if [[ -d lib ]]; then
    if php_hits="$(grep -rnE '#include[[:space:]]*[<"](php|zend|Zend/|ext/standard/)' lib \
            --exclude-dir=_deps)"; then
        fail "lib/ includes PHP/zend headers (SPEC §4):"
        printf '%s\n' "${php_hits}" >&2
    else
        echo "  ok"
    fi
fi

echo "== 4. no Mojang-derived artefacts in the tree =="
# SPEC §12: the repo ships scripts, never data.
artefacts="$(find . \
    -path ./.git -prune -o \
    -path ./build -prune -o \
    -path './cmake-build-*' -prune -o \
    -path '*/_deps' -prune -o \
    -type f \( -name '*.jar' -o -name '*.mca' -o -name '*.mcr' -o -name '*.nbt' \) \
    -print 2>/dev/null)"
if [[ -n "${artefacts}" ]]; then
    fail "Mojang-derived artefacts present in the working tree (SPEC §12):"
    printf '%s\n' "${artefacts}" >&2
else
    echo "  ok"
fi

echo "== 5. determinism flags still applied =="
det_module="cmake/StratumDeterminism.cmake"
det_failures=${failures}
if [[ ! -f "${det_module}" ]]; then
    fail "${det_module} is missing; determinism flags are unenforced."
else
    for required in '-ffp-contract=off' '/fp:precise'; do
        if ! grep -qF -- "${required}" "${det_module}"; then
            fail "${det_module} no longer applies '${required}' (SPEC §5.4)."
        fi
    done
    if ! grep -qF 'stratum_set_determinism' cmake/StratumDeterminism.cmake; then
        fail "stratum_set_determinism() is gone; targets would build unpinned."
    fi
    [[ ${failures} -eq ${det_failures} ]] && echo "  ok"
fi

if [[ ${failures} -gt 0 ]]; then
    printf '\n%d determinism/policy check(s) failed.\n' "${failures}" >&2
    exit 1
fi

echo
echo "all determinism and provenance checks passed"
