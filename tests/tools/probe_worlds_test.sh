#!/usr/bin/env bash
# Stratum — tools/probe-worlds against stub generators, offline.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# No server is started. The real table is linted against the real
# tools/analysis and ci.yml; then the tool is copied into a scratch
# repository whose every generator is a stub writing the corpora the table
# says it writes, so `generate`, `verify` and `key` run end to end. `verify`
# is what stands between a half-generated shard and CI's cache, and `key` is
# what decides which shards a script change regenerates.
# Plain bash 3.2 and python3: this runs on the macOS legs too.
set -euo pipefail
command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping"; exit 77; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# CI runs this inside a build leg: its summary, keys and version are not ours.
unset GITHUB_ACTIONS GITHUB_STEP_SUMMARY PROBE_CACHE_EPOCH PROBE_JAVA MINECRAFT_VERSION
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

expect() {  # expect <exit code> <what> <pattern or -> <command...>
    local want="$1" what="$2" pattern="$3"; shift 3
    local got=0
    "$@" > "${work}/out" 2>&1 || got=$?
    if [[ "${got}" -ne "${want}" ]] || { [[ "${pattern}" != - ]] && ! grep -qF -- "${pattern}" "${work}/out"; }; then
        echo "FAIL: ${what}: exit ${got} (wanted ${want}), output:"; cat "${work}/out"; exit 1
    fi
    echo "ok: ${what}"
}

real="${repo_root}/tools/probe-worlds"
expect 0 "the table agrees with tools/analysis and ci.yml" "ok:" "${real}" lint
expect 2 "a subcommand is required" "usage" "${real}"
expect 1 "refuses to start a server without --accept-eula" "needs --accept-eula" "${real}" generate
expect 2 "refuses an unknown shard" "unknown shard" "${real}" verify --shard nowhere

# --- a scratch repository: the tool, stub generators, a fake jar and worldgen.
fake="${work}/repo"
pw="${fake}/tools/probe-worlds"
probes="${fake}/.fixtures/1.21.11/probes"
mkdir -p "${fake}/tools/analysis" "${fake}/.github/workflows" "${work}/bin" \
         "${fake}/.fixtures/1.21.11/worldgen/noise_settings" "${fake}/.fixtures/1.21.11/worldgen/biome"
cp "${real}" "${pw}"
: > "${fake}/.fixtures/1.21.11/server-1.21.11.jar"
echo '{}' > "${fake}/.fixtures/1.21.11/worldgen/noise_settings/overworld.json"
for tool in java jq curl unzip; do printf '#!/bin/sh\nexit 0\n' > "${work}/bin/${tool}"; chmod +x "${work}/bin/${tool}"; done
export PATH="${work}/bin:${PATH}"
"${pw}" inventory > "${work}/inventory.json"
# Counted off the table rather than written in, so a new unit does not break
# the checks that it is generated: every unit, the aquifer shard's, and the
# ones aquifer-probes.sh's --only 'aquifer-*' selects (unit name or generator
# basename, as the tool matches them).
counts="$(python3 -c '
import fnmatch, json, os, sys
units = json.load(open(sys.argv[1]))["units"]
aquifer = [u for u in units if fnmatch.fnmatch(u["name"], "aquifer-*")
           or fnmatch.fnmatch(os.path.basename(u["argv"][0]), "aquifer-*")]
print(len(units), len(aquifer), sum(u["shard"] == "aquifer" for u in units),
      len(json.load(open(sys.argv[1]))["shards"]))
' "${work}/inventory.json")"
read -r all_units aquifer_units aquifer_shard_units shard_count <<< "${counts}"

# A region file the way the server writes one, as far as `verify` reads it:
# the 4 KiB location table, then each chunk a length, compression 2 (zlib)
# and an NBT compound holding its Status. Every chunk here shares one
# sector. region.py <path> [status] [chunks present]
cat > "${work}/region.py" <<'PY'
import pathlib, sys, zlib
def write(path, status='minecraft:full', present=1024):
    nbt = (b'\x0a\x00\x00\x03\x00\x0bDataVersion\x00\x00\x11\x9b\x08\x00\x06Status'
           + len(status).to_bytes(2, 'big') + status.encode() + b'\x00')
    payload = zlib.compress(nbt)
    body = (len(payload) + 1).to_bytes(4, 'big') + b'\x02' + payload
    location = ((2 << 8) | 1).to_bytes(4, 'big')
    table = b''.join(location if i < present else bytes(4) for i in range(1024))
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(table + bytes(4096) + body + bytes(4096 - len(body)))
if __name__ == '__main__':
    write(sys.argv[1], *(sys.argv[2:3] or []), *(int(n) for n in sys.argv[3:4]))
PY

# What every stub runs: finds its unit by script and arguments and writes
# what the table says that unit writes. FAKE_FAIL=<unit> makes the unit exit
# 1 having written nothing, FAKE_PARTIAL=<unit> after writing everything;
# FAKE_THAWED=<corpus> records that corpus as not frozen.
cat > "${work}/stub.py" <<'PY'
import json, os, pathlib, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import region
inventory, script, args = json.load(open(sys.argv[1])), sys.argv[2], sys.argv[3:]
assert args and args[-1] == '--accept-eula', f'--accept-eula must come last: {args}'
assert 'STRATUM_FIXTURES_DIR' not in os.environ, 'STRATUM_FIXTURES_DIR reached a generator'
unit = next(u for u in inventory['units']
            if os.path.basename(u['argv'][0]) == script and u['argv'][1:] == args[:-1])
if os.environ.get('FAKE_FAIL') == unit['name']:
    sys.exit(1)
probes = pathlib.Path('.fixtures/1.21.11/probes')
for out in unit['outputs']:
    if out.get('root') == 'regions':
        region.write(pathlib.Path('.fixtures/1.21.11/regions') / out['file'])
        continue
    root = probes / out['dir']
    root.mkdir(parents=True, exist_ok=True)
    if out.get('density'):
        for entry in ('one', 'two'):
            (root / entry).mkdir(exist_ok=True)
            (root / entry / 'r.0.0.mca').write_bytes(b'\0')
        (root / 'spec.json').write_text(json.dumps([{'name': 'one'}, {'name': 'two'}]))
        (root / 'manifest.json').write_text(json.dumps(
            {'seed': out['seed'], 'region': 'r.0.0.mca',
             'ticks_frozen': os.environ.get('FAKE_THAWED') != out['dir']}))
        continue
    for name in out['files']:
        # A unit's files may sit in subdirectories (aquifer-presets' three
        # dimensions), as the real generators write them.
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(b'\0')
    if 'record' in out:
        spec = out['record']
        (root / spec['file']).write_text(json.dumps(
            {'seed': spec['seed'], 'ticks_frozen': spec.get('frozen', False),
             'version': inventory['version'], 'variants': [{}] * spec.get('variants', 0)}))
if os.environ.get('FAKE_PARTIAL') == unit['name']:
    sys.exit(1)
PY

# One stub per generator (a density one names density-probe.sh, as the real
# ones do), a stub for every exempt script, and a ci.yml with the shards.
python3 - "${work}/inventory.json" "${fake}" "${work}/stub.py" <<'PY'
import json, pathlib, sys
inventory, fake, stub = json.load(open(sys.argv[1])), pathlib.Path(sys.argv[2]), sys.argv[3]
analysis = fake / 'tools/analysis'
for path in sorted({u['argv'][0] for u in inventory['units']}):
    density = any(out.get('density') for u in inventory['units'] if u['argv'][0] == path
                  for out in u['outputs'])
    text = '#!/bin/sh\n'
    if density:
        text += ': tools/analysis/density-probe.sh\n'
    if path == 'tools/fetch-vanilla':
        text += f'readonly DEFAULT_SEEDS="{",".join(inventory["golden_seeds"])}"\n'
    text += f'exec python3 "{stub}" "{sys.argv[1]}" "$(basename "$0")" "$@"\n'
    (fake / path).write_text(text)
    (fake / path).chmod(0o755)
for name in inventory['exempt']:
    (analysis / name).write_text('#!/bin/sh\n# the harness, or a script CI does not run\n')
    (analysis / name).chmod(0o755)
lookup = ('          path: ${{ steps.key.outputs.path }}\n'
          '          key: ${{ steps.key.outputs.key }}\n')
restores = ''.join(f'          path: .fixtures/${{{{ env.MINECRAFT_VERSION }}}}/{inventory["roots"][s]}\n'
                   f'          key: ${{{{ steps.probe-keys.outputs.{s} }}}}\n'
                   for s in inventory['shards'])
(fake / '.github/workflows/ci.yml').write_text(
    f'env:\n  MINECRAFT_VERSION: "{inventory["version"]}"\njobs:\n  probes:\n'
    f'    strategy:\n      matrix:\n        shard: [{", ".join(inventory["shards"])}]\n'
    f'{lookup}{restores}')
PY
export STRATUM_FIXTURES_DIR="${work}/elsewhere"   # must never reach a generator

expect 0 "the scratch repository lints clean" "ok:" "${pw}" lint
printf '#!/bin/sh\n' > "${fake}/tools/analysis/new-thing-probe.sh"
expect 1 "a probe script in neither list fails lint" "new-thing-probe.sh is neither a unit nor exempt" \
    "${pw}" lint
rm "${fake}/tools/analysis/new-thing-probe.sh"

# The goldens: lint holds them to fetch-vanilla's seeds, to the options that
# keep them what the cases score, and to the tree CI caches each shard as.
# Each fault is put back before the next.
cp "${pw}" "${work}/saved"
sed -i.bak "s/'--dimensions', 'nether', '--heap', '4G')/'--dimensions', 'nether', '--heap', '4G', '--with-structures')/" "${pw}"
expect 1 "a golden unit may not pass --with-structures" "--with-structures is not one a golden may pass" \
    "${pw}" lint
cp "${work}/saved" "${pw}"
sed -i.bak "s/'--dimensions', 'nether', '--heap', '4G')/'--dimensions', 'nether', '--heap', '4G', '--no-freeze-ticks')/" "${pw}"
expect 1 "a golden unit may not thaw the world" "--no-freeze-ticks is not one a golden may pass" "${pw}" lint
cp "${work}/saved" "${pw}"
cp "${fake}/tools/fetch-vanilla" "${work}/saved"
sed -i.bak 's/^readonly DEFAULT_SEEDS="0,/readonly DEFAULT_SEEDS="7,/' "${fake}/tools/fetch-vanilla"
expect 1 "the golden seeds are fetch-vanilla's own" "but tools/fetch-vanilla's DEFAULT_SEEDS is 7," "${pw}" lint
cp "${work}/saved" "${fake}/tools/fetch-vanilla"
cp "${fake}/.github/workflows/ci.yml" "${work}/saved"
sed -i.bak 's|/regions$|/probes|' "${fake}/.github/workflows/ci.yml"
# shellcheck disable=SC2016 # the workflow's own ${{ }}, verbatim
expect 1 "a goldens shard restored into probes/ fails lint" \
    'restores shard goldens-nether into .fixtures/${{ env.MINECRAFT_VERSION }}/probes' "${pw}" lint
cp "${work}/saved" "${fake}/.github/workflows/ci.yml"
sed -i.bak 's|path: ${{ steps.key.outputs.path }}|path: .fixtures/1.21.11/probes|' "${fake}/.github/workflows/ci.yml"
expect 1 "the probes job caches the tree key names" "probes job caches .fixtures/1.21.11/probes" "${pw}" lint
cp "${work}/saved" "${fake}/.github/workflows/ci.yml"
rm -f "${pw}.bak" "${fake}/tools/fetch-vanilla.bak" "${fake}/.github/workflows/ci.yml.bak"
expect 0 "every fault put back, the scratch repository lints clean again" "ok:" "${pw}" lint

# --- generate
expect 0 "generates one shard, then checks it" "output(s) of ${aquifer_shard_units} unit(s) present" \
    "${pw}" generate --shard aquifer --accept-eula
test -s "${probes}/comb_999/two/r.0.0.mca"
test ! -e "${probes}/capfloor_s42"
mkdir -p "${probes}/pslvar/stale"
expect 0 "replaces a unit's corpus whole" "of 1 unit(s) present" "${pw}" generate --only pslvar --accept-eula
test ! -e "${probes}/pslvar/stale"
rm -r "${probes}/lowsea"
echo old > "${probes}/fluidtype/old"
expect 1 "names a failed unit, after running the rest" "1 of ${aquifer_shard_units} unit(s) failed: fluidtype" \
    env FAKE_FAIL=fluidtype "${pw}" generate --shard aquifer --accept-eula
test -s "${probes}/lowsea/manifest.json"
if ! { test -s "${probes}/fluidtype/old" && test -s "${probes}/fluidtype/one/r.0.0.mca"; }; then
    echo "FAIL: a failed unit lost the corpus it had"; exit 1
fi
test ! -e "${probes}.previous" || { echo "FAIL: probes.previous left behind"; exit 1; }
echo "ok: a failed unit keeps the corpus it had"
for corpus in comb_42 comb_7 comb_12345 comb_999; do echo old > "${probes}/${corpus}/old"; done
expect 1 "a unit that fails part-way is undone whole" "1 of 1 unit(s) failed: comb" \
    env FAKE_PARTIAL=comb "${pw}" generate --only comb --accept-eula
for corpus in comb_42 comb_7 comb_12345 comb_999; do
    test -s "${probes}/${corpus}/old" || { echo "FAIL: ${corpus} not put back"; exit 1; }
done
rm -r "${probes}/depthgate"
expect 1 "a first generation that fails leaves nothing half-written" "failed: depthgate" \
    env FAKE_PARTIAL=depthgate "${pw}" generate --only depthgate --accept-eula
test ! -e "${probes}/depthgate" || { echo "FAIL: a half-written depthgate was kept"; exit 1; }
echo "ok: a failed unit's half-written corpus goes"
# A run killed outright: the corpus set aside is the last whole one.
mkdir -p "${probes}.previous" && mv "${probes}/lowsea" "${probes}.previous/lowsea"
echo old > "${probes}.previous/lowsea/old"
mkdir -p "${probes}/lowsea" && echo half > "${probes}/lowsea/half"
expect 1 "after a killed run, a failure restores the copy set aside" "failed: lowsea" \
    env FAKE_FAIL=lowsea "${pw}" generate --only lowsea --accept-eula
if ! { test -s "${probes}/lowsea/old" && test ! -e "${probes}/lowsea/half" \
        && test ! -e "${probes}.previous"; }; then
    echo "FAIL: the copy set aside by a killed run was not restored"; exit 1
fi
echo "ok: a killed run's half-written corpus is not taken for the previous one"
expect 0 "a success replaces the copy set aside" "of 1 unit(s) present" \
    "${pw}" generate --only lowsea --accept-eula
if ! { test ! -e "${probes}/lowsea/old" && test ! -e "${probes}.previous"; }; then
    echo "FAIL: a successful unit kept its previous corpus"; exit 1
fi
expect 1 "refuses what it generated unfrozen" "lowsea/manifest.json: ticks_frozen is False" \
    env FAKE_THAWED=lowsea "${pw}" generate --only lowsea --accept-eula
expect 0 "generates every shard" "output(s) of ${all_units} unit(s) present" "${pw}" generate --accept-eula

# --- the region goldens: files under regions/, each checked whole
regions="${fake}/.fixtures/1.21.11/regions"
test -s "${regions}/seed--4172144997902289642/overworld/r.4.3.mca"
test -s "${regions}/seed--9223372036854775808/nether/r.0.0.mca"
expect 0 "a goldens shard verifies" "golden region(s), every chunk at full status" \
    "${pw}" verify --shard goldens-overworld
python3 "${work}/region.py" "${regions}/seed-42/end/r.0.0.mca" minecraft:noise
expect 1 "a golden region short of full status fails" \
    "regions/seed-42/end/r.0.0.mca: 1024 chunk(s) at status 'minecraft:noise'" "${pw}" verify
python3 "${work}/region.py" "${regions}/seed-42/end/r.0.0.mca" minecraft:full 1000
expect 1 "a golden region missing chunks fails" "regions/seed-42/end/r.0.0.mca: 24 of 1024 chunks missing" \
    "${pw}" verify --shard goldens-overworld
printf 'not a region' > "${regions}/seed-42/end/r.0.0.mca"
expect 1 "a truncated golden region fails" "regions/seed-42/end/r.0.0.mca: shorter than its 8 KiB header" \
    "${pw}" verify --only goldens-overworld-end
expect 0 "other shards do not see a goldens fault" "present" "${pw}" verify --shard goldens-nether
expect 0 "its shard regenerates it" "golden region(s), every chunk at full status" \
    "${pw}" generate --shard goldens-overworld --accept-eula
echo old > "${regions}/seed-0/nether/r.0.0.mca"
rm "${regions}/seed-1/nether/r.0.0.mca"
expect 1 "a failed golden unit is undone" "1 of 1 unit(s) failed: goldens-nether" \
    env FAKE_PARTIAL=goldens-nether "${pw}" generate --shard goldens-nether --accept-eula
if ! { grep -q old "${regions}/seed-0/nether/r.0.0.mca" && test ! -e "${regions}/seed-1/nether/r.0.0.mca" \
        && test ! -e "${regions}.previous"; }; then
    echo "FAIL: a failed golden unit did not leave the regions it found"; exit 1
fi
echo "ok: a failed golden unit leaves the regions it found, and none it half-wrote"
expect 0 "the goldens regenerate" "of 1 unit(s) present" "${pw}" generate --only goldens-nether --accept-eula
cp "${repo_root}/tools/analysis/aquifer-probes.sh" "${fake}/tools/analysis/aquifer-probes.sh"
expect 0 "aquifer-probes.sh runs the table's aquifer units" "of ${aquifer_units} unit(s) present" \
    "${fake}/tools/analysis/aquifer-probes.sh" --accept-eula
expect 0 "in Actions, groups each unit's log" "::group::legseed_s42: tools/analysis/legacy-seed-probe.sh 42 --accept-eula" \
    env GITHUB_ACTIONS=true GITHUB_STEP_SUMMARY="${work}/summary" "${pw}" generate --shard legacy --accept-eula
grep -q '^| legseed_s31337 | ok | ' "${work}/summary" || { echo "FAIL: no step summary row"; exit 1; }
echo "ok: in Actions, writes a row per unit to the step summary"

# --- verify, one fault at a time, each restored before the next
expect 0 "a whole tree passes" "every density probe frozen" "${pw}" verify
expect 1 "nothing passes elsewhere" "missing or empty" "${pw}" verify --fixtures "${work}/elsewhere"
cp "${probes}/capfloor_s42/manifest.json" "${work}/saved"
printf '{"seed": 7, "ticks_frozen": true, "region": "r.0.0.mca"}' > "${probes}/capfloor_s42/manifest.json"
expect 1 "a corpus from another seed fails" "capfloor_s42/manifest.json: seed 7, expected 42" "${pw}" verify
printf '{"seed": 42, "region": "r.0.0.mca"}' > "${probes}/capfloor_s42/manifest.json"
expect 1 "an unfrozen corpus fails" "ticks_frozen is None" "${pw}" verify --shard capfloor
cp "${work}/saved" "${probes}/capfloor_s42/manifest.json"
rm "${probes}/waterlava_s31337/two/r.0.0.mca"
expect 1 "a missing entry fails" "waterlava_s31337/two/r.0.0.mca: missing or empty" "${pw}" verify
: > "${probes}/waterlava_s31337/two/r.0.0.mca"
expect 1 "an empty region fails" "waterlava_s31337/two/r.0.0.mca: missing or empty" "${pw}" verify
printf '\0' > "${probes}/waterlava_s31337/two/r.0.0.mca"
cp "${probes}/apsb3/spec.json" "${work}/saved"
echo '[]' > "${probes}/apsb3/spec.json"
expect 1 "a spec naming no entries fails" "apsb3/spec.json names no entries" "${pw}" verify
cp "${work}/saved" "${probes}/apsb3/spec.json"
rm -r "${probes}/end-islands/seed-42"
expect 1 "a missing own-layout corpus fails" "end-islands/seed-42/r.64.0.mca" "${pw}" verify
expect 0 "other shards do not see it" "present" "${pw}" verify --shard legacy
expect 0 "the end shard regenerates it" "present" "${pw}" generate --shard end --accept-eula
printf '{"seed": 42, "version": "1.21.11", "variants": [{}]}' > "${probes}/inline-noise/verdicts.json"
expect 1 "a half-written verdicts file fails" "11 variants" "${pw}" verify --only inline-noise

# --- cache keys
expect 1 "key needs the epoch and the JDK" "must be set" env -u PROBE_CACHE_EPOCH "${pw}" key aquifer
export PROBE_CACHE_EPOCH=1 PROBE_JAVA=25 MINECRAFT_VERSION=1.21.11
expect 1 "key refuses a version the table is not for" "but this table is for 1.21.11" \
    env MINECRAFT_VERSION=1.21.12 "${pw}" key aquifer
expect 0 "a key names its epoch, version, JDK and shard" "key=probe-worlds-e1-1.21.11-jdk25-water-" \
    "${pw}" key water
expect 0 "a probe shard caches probes/" "path=.fixtures/1.21.11/probes" "${pw}" key water
expect 0 "a goldens shard caches regions/" "path=.fixtures/1.21.11/regions" "${pw}" key goldens-nether
"${pw}" keys > "${work}/keys.before"
[[ "$(wc -l < "${work}/keys.before")" -eq "${shard_count}" ]] || { echo "FAIL: keys printed $(wc -l < "${work}/keys.before") lines, for ${shard_count} shards"; exit 1; }
echo "# one more line" >> "${fake}/tools/analysis/aquifer-lowsea-probe.sh"
"${pw}" keys > "${work}/keys.after"
changed="$(diff "${work}/keys.before" "${work}/keys.after" | grep -c '^>' || true)"
grep -q '^> aquifer=' <(diff "${work}/keys.before" "${work}/keys.after")
[[ "${changed}" -eq 1 ]] || { echo "FAIL: a generator edit changed ${changed} keys, wanted 1"; exit 1; }
echo "ok: editing one generator changes only its shard's key"
echo "# one more line" >> "${fake}/tools/analysis/density-probe.sh"
"${pw}" keys > "${work}/keys.harness"
# Exactly the shards with a unit that density-probe.sh writes for, read off
# the table: the set moves when units do, the property does not.
want="$(python3 -c '
import json, sys
units = json.load(open(sys.argv[1]))["units"]
print(" ".join(sorted({u["shard"] for u in units
                       if any(o.get("density") for o in u["outputs"])})))
' "${work}/inventory.json")"
got="$({ diff "${work}/keys.after" "${work}/keys.harness" || true; } \
       | sed -n 's/^> \([^=]*\)=.*/\1/p' \
       | sort | tr '\n' ' ' | sed 's/ $//')"
[[ "${got}" == "${want}" ]] || {
    echo "FAIL: a harness edit changed the keys of [${got}], wanted [${want}]"; exit 1; }
echo "ok: editing density-probe.sh changes exactly the shards that run it (${want})"
echo "# one more line" >> "${fake}/tools/fetch-vanilla"
"${pw}" keys > "${work}/keys.fetch"
want="$(python3 -c '
import json, sys
units = json.load(open(sys.argv[1]))["units"]
print(" ".join(sorted({u["shard"] for u in units if u["argv"][0] == "tools/fetch-vanilla"})))
' "${work}/inventory.json")"
got="$({ diff "${work}/keys.harness" "${work}/keys.fetch" || true; } \
       | sed -n 's/^> \([^=]*\)=.*/\1/p' \
       | sort | tr '\n' ' ' | sed 's/ $//')"
[[ -n "${want}" && "${got}" == "${want}" ]] || {
    echo "FAIL: a fetch-vanilla edit changed the keys of [${got}], wanted [${want}]"; exit 1; }
echo "ok: editing tools/fetch-vanilla changes exactly the goldens shards' keys (${want})"
expect 0 "the epoch is in every key" "aquifer=probe-worlds-e2-" env PROBE_CACHE_EPOCH=2 "${pw}" keys

echo "probe-worlds: all cases passed"
