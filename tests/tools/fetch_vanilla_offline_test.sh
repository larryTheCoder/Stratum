#!/usr/bin/env bash
# Stratum — offline tests for tools/fetch-vanilla.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Hermetic: builds a synthetic "server jar" and never touches the network, so
# it runs in the normal unit suite. The paths that do need the network (the
# manifest trust chain) are exercised by the conformance job, which is where
# real fixtures belong.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fetch="${repo_root}/tools/fetch-vanilla"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

failures=0
check() {
    local what="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  ok: ${what}"
    else
        echo "  FAILED: ${what}" >&2
        failures=$((failures + 1))
    fi
}

expect_failure() {
    local what="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  FAILED: ${what} (command succeeded but should not have)" >&2
        failures=$((failures + 1))
    else
        echo "  ok: ${what}"
    fi
}

# --- a synthetic server jar --------------------------------------------
jar_src="${work_dir}/jar-src"
mkdir -p "${jar_src}/data/minecraft/worldgen/noise_settings" \
         "${jar_src}/data/minecraft/worldgen/density_function/overworld" \
         "${jar_src}/net/minecraft"
echo '{"sea_level": 63}' > "${jar_src}/data/minecraft/worldgen/noise_settings/overworld.json"
echo '{"type": "minecraft:constant"}' \
    > "${jar_src}/data/minecraft/worldgen/density_function/overworld/base_3d_noise.json"
echo 'not worldgen' > "${jar_src}/net/minecraft/Main.class"
jar_file="${work_dir}/fake-server.jar"
(cd "${jar_src}" && zip -qr "${jar_file}" .)

echo "== fetch-vanilla: help and dry run =="
check "--help exits 0" "${fetch}" --help
check "--dry-run performs no network access" "${fetch}" --dry-run --output "${work_dir}/dry"
expect_failure "an unknown option is rejected" "${fetch}" --nonsense

echo "== fetch-vanilla: extraction from a supplied jar =="
out="${work_dir}/out"
check "extracts from --jar without network" \
    "${fetch}" --jar "${jar_file}" --skip-verify --output "${out}"

check "noise_settings landed at the id-shaped path" \
    test -f "${out}/worldgen/noise_settings/overworld.json"
check "nested density_function path survived" \
    test -f "${out}/worldgen/density_function/overworld/base_3d_noise.json"
check "non-worldgen jar content was not extracted" \
    test ! -e "${out}/worldgen/net"
check "the staging directory was cleaned up" test ! -e "${out}/.extract"
check "a provenance record was written" test -f "${out}/PROVENANCE.json"

if command -v jq >/dev/null 2>&1; then
    counted="$(jq -r '.worldgen_files' "${out}/PROVENANCE.json")"
    if [[ "${counted}" == "2" ]]; then
        echo "  ok: provenance counts both worldgen files"
    else
        echo "  FAILED: provenance counted '${counted}', expected 2" >&2
        failures=$((failures + 1))
    fi
fi

echo "== fetch-vanilla: bundler jars (every release since 1.18) =="
# The published server jar is a launcher: the real server, and the worldgen
# data with it, sits at META-INF/versions/<id>/server-<id>.jar, with its
# SHA-256 in META-INF/versions.list. A synthetic plain jar happily passed
# these tests while the real thing failed, so the layout is covered here.
bundle_src="${work_dir}/bundle-src"
mkdir -p "${bundle_src}/META-INF/versions/1.21.11"
inner_jar="${bundle_src}/META-INF/versions/1.21.11/server-1.21.11.jar"
(cd "${jar_src}" && zip -qr "${inner_jar}" .)
inner_sha256="$(sha256sum "${inner_jar}" | cut -d' ' -f1)"
printf '%s\t1.21.11\t1.21.11/server-1.21.11.jar\n' "${inner_sha256}" \
    > "${bundle_src}/META-INF/versions.list"
bundler_jar="${work_dir}/bundler.jar"
(cd "${bundle_src}" && zip -qr "${bundler_jar}" .)

bundler_out="${work_dir}/bundler-out"
bundler_log="${work_dir}/bundler.log"
check "extracts worldgen from a bundler jar" \
    env sh -c "'${fetch}' --jar '${bundler_jar}' --skip-verify --output '${bundler_out}' \
        > '${bundler_log}' 2>&1"
check "found the data in the nested server jar" \
    test -f "${bundler_out}/worldgen/noise_settings/overworld.json"
check "verified the nested jar's SHA-256" \
    grep -q "nested server jar verified" "${bundler_log}"
check "cleaned up the bundle staging directory" test ! -e "${bundler_out}/.bundle"

# Internal consistency is checked even under --skip-verify: that flag waives
# provenance, not integrity.
corrupt_src="${work_dir}/corrupt-src"
cp -r "${bundle_src}" "${corrupt_src}"
printf '%s\t1.21.11\t1.21.11/server-1.21.11.jar\n' \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    > "${corrupt_src}/META-INF/versions.list"
corrupt_jar="${work_dir}/corrupt.jar"
(cd "${corrupt_src}" && zip -qr "${corrupt_jar}" .)
expect_failure "a bundle whose versions.list disagrees with its contents is rejected" \
    "${fetch}" --jar "${corrupt_jar}" --skip-verify --output "${work_dir}/corrupt-out"

echo "== fetch-vanilla: loud refusals =="
empty_jar="${work_dir}/empty.jar"
(cd "${jar_src}/net" && zip -qr "${empty_jar}" .)
expect_failure "a jar with no worldgen entries is rejected" \
    "${fetch}" --jar "${empty_jar}" --skip-verify --output "${work_dir}/empty-out"

expect_failure "a missing jar path is rejected" \
    "${fetch}" --jar "${work_dir}/does-not-exist.jar" --skip-verify --output "${work_dir}/missing"

# The EULA is the user's to accept, never this script's.
# Captured rather than piped: `set -o pipefail` would otherwise make the
# pipeline inherit fetch-vanilla's (correct) non-zero exit status and mask
# grep's result.
eula_status=0
eula_output="$("${fetch}" --jar "${jar_file}" --skip-verify --output "${work_dir}/eula" \
    --generate-regions 2>&1)" || eula_status=$?
if [[ ${eula_status} -ne 0 ]] && grep -q "EULA" <<<"${eula_output}"; then
    echo "  ok: --generate-regions refuses without --accept-eula, naming the EULA"
else
    echo "  FAILED: --generate-regions did not refuse with an EULA message" >&2
    echo "${eula_output}" >&2
    failures=$((failures + 1))
fi

# With the EULA accepted it must still refuse loudly rather than quietly
# producing an empty fixture set.
regions_status=0
regions_output="$("${fetch}" --jar "${jar_file}" --skip-verify --output "${work_dir}/regions" \
    --generate-regions --accept-eula 2>&1)" || regions_status=$?
if [[ ${regions_status} -ne 0 ]] && grep -q "not implemented" <<<"${regions_output}"; then
    echo "  ok: --accept-eula still refuses loudly while region goldens are unimplemented"
else
    echo "  FAILED: --generate-regions --accept-eula did not refuse loudly" >&2
    echo "${regions_output}" >&2
    failures=$((failures + 1))
fi

if [[ ${failures} -gt 0 ]]; then
    printf '\n%d fetch-vanilla check(s) failed.\n' "${failures}" >&2
    exit 1
fi
echo
echo "all fetch-vanilla offline checks passed"
