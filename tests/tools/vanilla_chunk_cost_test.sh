#!/usr/bin/env bash
# Stratum — tools/analysis/vanilla-chunk-cost.py's reading, on synthetic events.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# No server is started. The events below are written here, in the shape
# `jfr print --json` gives the server's minecraft.ChunkGeneration events, on
# a schedule whose answers are known: one worker that queues stages behind
# each other (so an event's own duration is not its cost), an event of
# another level and another chunk to be filtered out, a stage that waits on
# an idle worker, and `full` on the server thread. The second recording adds
# a second worker, and the pin must be reported broken. Then the refusals
# that keep the server and the baseline from running without --accept-eula.
# Plain bash 3.2 and python3: this runs on the macOS legs too.
set -euo pipefail
command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping"; exit 77; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tool="${repo_root}/tools/analysis/vanilla-chunk-cost.py"
baseline="${repo_root}/tools/perf-baseline"
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

# events <file> <zone> <second worker: 0|1>. Times are milliseconds after
# midnight; the zone only changes how the instants are spelled.
events() {
    python3 - "$@" <<'PY'
import json, sys
path, zone, second = sys.argv[1], sys.argv[2], sys.argv[3] == '1'
offset_hours = 8 if zone == '+08:00' else 0
def instant(ms):
    seconds, nanos = divmod(round(ms * 1_000_000), 1_000_000_000)
    hours = (seconds // 3600 + offset_hours) % 24
    minutes, secs = divmod(seconds % 3600, 60)
    stamp = f'2026-01-01T{hours:02d}:{minutes:02d}:{secs:02d}.{nanos:09d}'
    return stamp + ('Z' if zone == 'Z' else zone)
def event(status, x, z, start, duration, thread='Worker-Main-1', level='minecraft:overworld'):
    whole, nanos = divmod(round(duration * 1_000_000), 1_000_000_000)
    return {'type': 'minecraft.ChunkGeneration', 'values': {
        'startTime': instant(start), 'duration': f'PT{whole}.{nanos:09d}S',
        'eventThread': {'javaName': thread, 'osName': thread},
        'chunkPosX': x, 'chunkPosZ': z, 'status': f'minecraft:{status}', 'level': level}}
recorded = [
    event('biomes', 0, 0, 0, 4),            # costs 4
    event('biomes', 5, 5, 0, 7),            # outside the area; queued, costs 3
    event('noise', 0, 0, 1, 16),            # scheduled at 1, done at 17: costs 17 - 7 = 10
    event('noise', 0, 0, 17, 2, level='minecraft:the_end'),  # another level
    event('surface', 0, 0, 19, 5),          # costs 5
    event('features', 0, 0, 24.5, 1.5),     # the worker was idle from 24: costs 1.5
    event('full', 0, 0, 26, 0.8, thread='Server thread'),
    {'type': 'jdk.CPULoad', 'values': {}},  # not a chunk event: ignored
]
if second:
    recorded.append(event('carvers', 0, 0, 30, 1, thread='Worker-Main-2'))
json.dump({'recording': {'events': recorded}}, open(path, 'w'))
PY
}

check() {  # check <json> <python expression over r> <what>
    python3 - "$1" "$2" "$3" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
if not eval(sys.argv[2]):
    print(f'FAIL: {sys.argv[3]}: {sys.argv[2]} is false in'); print(json.dumps(r, indent=1)[:4000])
    sys.exit(1)
print(f'ok: {sys.argv[3]}')
PY
}

events "${work}/one.json" Z 0
expect 0 "a recording is read" "comparable median    19.00" \
    "${tool}" analyze "${work}/one.json" --area 0,0,0 --json "${work}/one.out.json"
check "${work}/one.out.json" "abs(r['stages']['biomes']['median_ms'] - 4) < 1e-6" \
    "a stage that ran at once costs its duration"
check "${work}/one.out.json" "abs(r['stages']['noise']['median_ms'] - 10) < 1e-6" \
    "a queued stage costs the worker's time since the previous completion"
check "${work}/one.out.json" "abs(r['stages']['noise']['raw_median_ms'] - 16) < 1e-6" \
    "and its raw duration, queueing included, is kept beside it"
check "${work}/one.out.json" "abs(r['stages']['features']['median_ms'] - 1.5) < 1e-6" \
    "a stage on an idle worker costs its duration, not the idle time before it"
check "${work}/one.out.json" "abs(r['comparable']['median_ms'] - 19) < 1e-6 and r['comparable']['chunks'] == 1" \
    "comparable is biomes + noise + surface of the area's chunk, the other level's noise excluded"
check "${work}/one.out.json" "abs(r['comparable']['chunks_per_s'] - 1000 / 19) < 1e-6" \
    "chunks/s is 1000 / mean"
check "${work}/one.out.json" "r['stages']['full']['raw_only'] and abs(r['stages']['full']['median_ms'] - 0.8) < 1e-6" \
    "full is reported raw, from the server thread"
check "${work}/one.out.json" "r['complete'] and r['missing'] == {'biomes': 0, 'noise': 0, 'surface': 0, 'full': 0}" \
    "the area is complete"
check "${work}/one.out.json" "r['pin_held'] and r['generation_threads'] == ['Worker-Main-1']" \
    "one worker committed every stage but full: the pin held"
check "${work}/one.out.json" "abs(r['threads']['Worker-Main-1']['busy_s'] - 0.0255) < 1e-9" \
    "the worker's busy time is every estimate on it, other levels and chunks included"

events "${work}/two.json" +08:00 1
expect 0 "an instant with an offset reads as the same instant" "comparable median    19.00" \
    "${tool}" analyze "${work}/two.json" --area 0,0,0 --json "${work}/two.out.json"
expect 0 "a second worker is reported" "THE PIN DID NOT HOLD" \
    "${tool}" analyze "${work}/two.json" --area 0,0,0
check "${work}/two.out.json" "not r['pin_held'] and r['worker_threads'] == ['Worker-Main-1', 'Worker-Main-2']" \
    "a second worker committing generation breaks the pin"
expect 0 "two workers are within a pin of two" "comparable" \
    "${tool}" analyze "${work}/two.json" --area 0,0,0 --threads 2 --json "${work}/three.out.json"
check "${work}/three.out.json" "r['pin_held']" "and a pin of two holds"
expect 0 "an area chunk with no events is reported" "INCOMPLETE" \
    "${tool}" analyze "${work}/one.json" --area 0,0,1 --json "${work}/four.out.json"
check "${work}/four.out.json" "not r['complete'] and r['missing']['noise'] == 8" \
    "eight of the nine area chunks have no noise event"

expect 1 "run refuses without --accept-eula" "needs --accept-eula" "${tool}" run
expect 2 "an area needs three numbers" "an area is cx,cz,r" "${tool}" analyze x --area 1,2
expect 1 "the baseline refuses the vanilla half without --accept-eula" "needs --accept-eula" \
    "${baseline}" --skip-stratum
expect 1 "the baseline refuses to do nothing" "both halves skipped" \
    "${baseline}" --skip-stratum --skip-vanilla
