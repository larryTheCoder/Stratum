#!/usr/bin/env bash
# Stratum — tools/analysis/vanilla-chunk-cost.py's reading, on synthetic events,
# and tools/perf-baseline's comparison, on synthetic halves.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# No server is started. The events below are written here, in the shape
# `jfr print --json` gives the server's minecraft.ChunkGeneration events, on
# a schedule whose answers are known: one worker that queues stages behind
# each other (so an event's own duration is not its cost), an event of
# another level and another chunk to be filtered out, a stage that waits on
# an idle worker, and `full` on the server thread. The second adds a
# fork-join spare whose stage overlaps the worker's: with one worker allowed
# the two are one timeline, with two they are two. The third commits a stage
# off the worldgen workers, which must be reported. Then the checks a
# recording is judged by (its pin, its estimates against its workers' CPU),
# the JVM's helper threads read from a jstack, the refusals that keep the
# server and the baseline from running without --accept-eula, a failed run
# that keeps its own directory and nothing else, and tools/perf-baseline's
# comparison from synthetic halves: its verdicts, its quiet-or-provisional
# label, and its refusal of halves that measured different things.
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

# events <file> <zone> <extra: none|spare|stray>. Times are milliseconds after
# midnight; the zone only changes how the instants are spelled.
events() {
    python3 - "$@" <<'PY'
import json, sys
path, zone, extra = sys.argv[1], sys.argv[2], sys.argv[3]
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
def collection(name, pauses, cause):
    return {'type': 'jdk.GarbageCollection',
            'values': {'name': name, 'sumOfPauses': pauses, 'cause': cause}}
recorded = [
    event('biomes', 0, 0, 0, 4),            # costs 4
    event('biomes', 5, 5, 0, 7),            # outside the area; queued, costs 3
    event('noise', 0, 0, 1, 16),            # scheduled at 1, done at 17: costs 17 - 7 = 10
    event('noise', 0, 0, 17, 2, level='minecraft:the_end'),  # another level
    event('surface', 0, 0, 19, 5),          # costs 5
    event('features', 0, 0, 24.5, 1.5),     # the worker was idle from 24: costs 1.5
    event('full', 0, 0, 26, 0.8, thread='Server thread'),
    {'type': 'jdk.CPULoad', 'values': {}},  # not a chunk event: ignored
    collection('G1Full', 'PT0.0125S', 'Heap Inspection Initiated GC'),
    collection('G1Full', 'PT0.0075S', 'Heap Inspection Initiated GC'),
    collection('G1New', 'PT0.002S', 'G1 Evacuation Pause'),
]
if extra == 'spare':
    # Scheduled at 24, done at 30: one timeline charges it 30 - 26 = 4, its
    # own thread 6.
    recorded.append(event('carvers', 0, 0, 24, 6, thread='Worker-Main-2'))
if extra == 'stray':
    recorded.append(event('light', 0, 0, 27, 1, thread='Light thread'))
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

events "${work}/one.json" Z none
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
check "${work}/one.out.json" "r['workers_only'] and r['commits'] == {'Worker-Main-1': 6}" \
    "one worker committed every stage but full"
check "${work}/one.out.json" "abs(r['worker_busy_s'] - 0.0255) < 1e-9" \
    "the worker's busy time is every estimate on it, other levels and chunks included"
check "${work}/one.out.json" "r['collections']['Heap Inspection Initiated GC'] == {'count': 2, 'pause_ms': 20.0, 'collectors': ['G1Full']} and r['collections']['G1 Evacuation Pause']['count'] == 1" \
    "the recording's garbage collections are counted by cause"

events "${work}/two.json" +08:00 spare
expect 0 "an instant with an offset reads as the same instant" "comparable median    19.00" \
    "${tool}" analyze "${work}/two.json" --area 0,0,0 --json "${work}/two.out.json"
check "${work}/two.out.json" "list(r['timelines']) == ['Server thread', 'Worker-Main-*']" \
    "with one worker allowed, a spare is on the worker's timeline"
check "${work}/two.out.json" "abs(r['stages']['carvers']['median_ms'] - 4) < 1e-6 and abs(r['worker_busy_s'] - 0.0295) < 1e-9" \
    "and is charged only what the worker had not already spent"
check "${work}/two.out.json" "r['workers_only'] and r['commits'] == {'Worker-Main-1': 6, 'Worker-Main-2': 1}" \
    "both workers' commits are counted"
expect 0 "with two allowed, two timelines" "comparable" \
    "${tool}" analyze "${work}/two.json" --area 0,0,0 --threads 2 --json "${work}/three.out.json"
check "${work}/three.out.json" "abs(r['stages']['carvers']['median_ms'] - 6) < 1e-6 and abs(r['worker_busy_s'] - 0.0315) < 1e-9" \
    "each charged on its own"

# The pin's CPU half and the estimates' check, as run() applies them after
# jstack: the one-worker recording's workers span 26 ms, so 25 ms of their CPU
# is one thread's worth and 40 ms is not; its estimates sum to 25.5 ms, so
# 25.5 ms of CPU backs them, 20 ms says the worker was descheduled for some of
# them, and 27 ms (still one thread's worth) says some of its work ended in no
# event.
pin() {  # pin <result json> <allowed> <worker:cpu_s,...>
    python3 - "${tool}" "$@" <<'PY'
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location('tool', sys.argv[1])
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
result = json.load(open(sys.argv[2]))
cpu = {name: float(value) for name, value in (p.split(':') for p in sys.argv[4].split(','))}
judged = tool.judge_pin(result, cpu, int(sys.argv[3]))
scaled = judged['comparable_cpu_scaled']
print(f"pin_held={judged['pin_held']} parallelism={judged['parallelism']:.3f} "
      f"ratio={judged['estimate_cpu_ratio']:.3f} scaled={scaled['median_ms']:.2f} "
      f"flags={' | '.join(judged['flags']) or '-'}")
PY
}
expect 0 "one thread's worth of CPU holds the pin" "pin_held=True parallelism=0.962" \
    pin "${work}/one.out.json" 1 Worker-Main-1:0.020,Worker-Main-2:0.005
expect 0 "more than one thread's worth breaks it" "pin_held=False parallelism=1.538" \
    pin "${work}/one.out.json" 1 Worker-Main-1:0.025,Worker-Main-2:0.015
expect 0 "which a pin of two allows" "pin_held=True" \
    pin "${work}/one.out.json" 2 Worker-Main-1:0.025,Worker-Main-2:0.015
expect 0 "estimates the CPU backs are not flagged" "ratio=1.000 scaled=19.00 flags=-" \
    pin "${work}/one.out.json" 1 Worker-Main-1:0.0255
expect 0 "estimates above the CPU are flagged: the wall overstates" \
    "ratio=1.275 scaled=14.90 flags=worker descheduled: wall overstates (the estimates are 1.275x" \
    pin "${work}/one.out.json" 1 Worker-Main-1:0.020
expect 0 "estimates below it are flagged too" \
    "pin_held=True parallelism=1.038 ratio=0.944 scaled=20.12 flags=the estimates miss worker CPU (they are 0.944x" \
    pin "${work}/one.out.json" 1 Worker-Main-1:0.027

# The JVM's helper threads from two jstacks, by native id: a compiler thread
# that ended between them is in the process total but no listed thread; one
# that started between them started from nothing; Worker-Main-2 here is a new
# thread under an old name.
helpers() {
    python3 - "${tool}" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('tool', sys.argv[1])
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
def stack(*threads):
    return '\n'.join(f'"{name}" #{i} [{nid}] daemon prio=5 os_prio=0 cpu={ms}ms elapsed=9.0s '
                     f'tid=0x0 nid={nid} waiting on condition  [0x0]\n   java.lang.Thread.State: X'
                     for i, (name, nid, ms) in enumerate(threads))
before = tool.parse_jstack(stack(('Worker-Main-1', 11, 1000), ('Worker-Main-2', 12, 500),
                                 ('C2 CompilerThread0', 13, 100), ('C2 CompilerThread1', 14, 50),
                                 ('GC Thread#0', 15, 10), ('Server thread', 16, 20)))
after = tool.parse_jstack(stack(('Worker-Main-1', 11, 3000), ('Worker-Main-2', 19, 400),
                                ('C2 CompilerThread0', 13, 600), ('C1 CompilerThread0', 17, 100),
                                ('GC Thread#0', 15, 310), ('G1 Conc#0', 18, 200),
                                ('VM Thread', 20, 100), ('VM Periodic Task Thread', 21, 5),
                                ('Server thread', 16, 120), ('IO-Worker-1', 22, 30),
                                ('JFR Recorder Thread', 23, 40), ('Netty Epoll IO #0', 24, 1)))
# C2 CompilerThread1 ended: /proc's last look (its name cut to 15
# characters) saw 0.3 s spent inside the recording.
kinds = tool.thread_kinds(before, after, 5.0, {'14': ('C2 CompilerThre', 0.3)})
workers = tool.worker_deltas(before, after)
share = tool.helper_shares(kinds)
expected = {'worker': 2.4, 'jit': 0.9, 'gc': 0.6, 'server': 0.1, 'io': 0.03, 'jfr': 0.04,
            'other': 0.006}
for kind, value in expected.items():
    assert abs(kinds[kind] - value) < 1e-9, (kind, kinds)
assert abs(kinds['ended'] - 0.3) < 1e-9, kinds
assert abs(kinds['unlisted'] - (5.0 - sum(expected.values()))) < 1e-9, kinds
assert workers == {'Worker-Main-1': 2.0, 'Worker-Main-2': 0.4}, workers
assert abs(share['jit'] - 0.375) < 1e-9 and abs(share['gc'] - 0.25) < 1e-9, share
assert 'ended' not in share, share
hexadecimal = '"VM Thread" os_prio=0 cpu=1.50ms elapsed=2.0s tid=0x1 nid=0x1f runnable'
assert tool.parse_jstack(hexadecimal) == {'31': ('VM Thread', 1.5)}
print('helpers told apart')
PY
}
expect 0 "the JVM's helper threads are told apart by kind, ended or renamed ones by native id" \
    "helpers told apart" helpers

events "${work}/stray.json" Z stray
expect 0 "a stage off the worldgen workers is reported" "NOT ONLY ON WORLDGEN WORKERS" \
    "${tool}" analyze "${work}/stray.json" --area 0,0,0 --json "${work}/stray.out.json"
check "${work}/stray.out.json" "not r['workers_only'] and r['stray_threads'] == ['Light thread']" \
    "and named"
expect 0 "and breaks the pin whatever the CPU" "pin_held=False" \
    pin "${work}/stray.out.json" 1 Worker-Main-1:0.010
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

# A run that fails keeps its own directory, inside --work, and leaves what was
# already in --work alone: a server that exits at once, from a stand-in JDK
# and an empty jar. run reads /proc, so Linux only.
if [[ -d /proc/self/task ]]; then
    mkdir -p "${work}/jdk/bin" "${work}/fixtures/1.21.11" "${work}/scratch/keepme"
    echo notes > "${work}/scratch/keepme/notes.txt"
    : > "${work}/fixtures/1.21.11/server-1.21.11.jar"
    cat > "${work}/jdk/bin/java" <<'SH'
#!/bin/sh
if [ "$1" = -version ]; then echo "stand-in 0" >&2; exit 0; fi
echo "stand-in server: exiting"
exit 1
SH
    printf '#!/bin/sh\nexit 1\n' > "${work}/jdk/bin/jfr"
    cp "${work}/jdk/bin/jfr" "${work}/jdk/bin/jstack"
    chmod +x "${work}/jdk/bin/java" "${work}/jdk/bin/jfr" "${work}/jdk/bin/jstack"
    expect 1 "a server that exits is a failure" "the server exited (see" \
        env STRATUM_FIXTURES_DIR="${work}/fixtures" "${tool}" run --accept-eula \
        --jdk "${work}/jdk" --work "${work}/scratch" --seeds 0 --dimensions overworld
    [[ -f "${work}/scratch/keepme/notes.txt" ]] \
        || { echo "FAIL: what --work held was removed"; ls -R "${work}/scratch"; exit 1; }
    echo "ok: what --work already held is left alone"
    named="$(sed -n 's/.*the server exited (see \(.*server\.log\)).*/\1/p' "${work}/out" | head -n 1)"
    if [[ -z "${named}" || ! -f "${named}" ]] || ! grep -qF "stand-in server: exiting" "${named}"; then
        echo "FAIL: the server.log the error names (${named}) is gone"; cat "${work}/out"; exit 1
    fi
    case "${named}" in
        "${work}/scratch/stratum-vanilla-cost-"*) ;;
        *) echo "FAIL: the run's directory is not its own, inside --work: ${named}"; exit 1 ;;
    esac
    grep -qF "kept ${work}/scratch/stratum-vanilla-cost-" "${work}/out" \
        || { echo "FAIL: the kept directory is not named"; cat "${work}/out"; exit 1; }
    echo "ok: the failed run keeps its own directory, and the server.log its error names"
fi

# The comparison alone, from an earlier run's two halves (--from): a Stratum
# end to end of 50 ms/chunk mean against vanilla's 10 is 5x, and between the
# owner's minimum (100 ms) and best case (35 ms).
# halves <dir> [key=value ...]: seeds, area (cx,cz,r), ratio (the vanilla
# recordings' estimates over their CPU), watch (none|quiet|busy), dims.
halves() {
    python3 - "$@" <<'PY'
import json, sys
directory = sys.argv[1]
options = dict(a.split('=', 1) for a in sys.argv[2:])
seeds = [int(s) for s in options.get('seeds', '0,42').split(',')]
area = [int(v) for v in options.get('area', '3,-4,1').split(',')]
vanilla_area = [int(v) for v in options.get('vanilla_area', ','.join(map(str, area))).split(',')]
ratio = float(options.get('ratio', '1.0'))
dims = options.get('dims', 'overworld').split(',')
def figure(mean):
    return {'available': True, 'chunks': 9, 'median_ms': mean, 'p10_ms': mean, 'p90_ms': mean,
            'mean_ms': mean, 'chunks_per_s': 1000 / mean}
stages = {s: {'wall': figure(m), 'cpu': figure(m)} for s, m in
          (('fill', 30.0), ('biomes', 19.0), ('encode', 1.0), ('e2e', 50.0))}
json.dump({'machine': {'optimised': True}, 'load_average_start': '0.10 0.10 0.10',
           'load_average_end': '0.20 0.10 0.10', 'seeds': seeds,
           'spiral': {'centre': area[:2], 'radius': area[2], 'chunks': 9},
           'dimensions': {d: {'spiral': stages} for d in dims}},
          open(directory + '/stratum.json', 'w'))
runs = [{'dimension': d, 'seed': s, 'pin_held': True, 'complete': True, 'missing': {},
         'worker_cpu_total_s': 1.0, 'worker_busy_s': ratio, 'other_cpu_cores': 0.1}
        for d in dims for s in seeds]
json.dump({'load_average_start': '0.30 0.10 0.10', 'load_average_end': '0.40 0.10 0.10',
           'seeds': seeds, 'area': vanilla_area, 'runs': runs,
           'dimensions': {d: {'comparable': figure(10.0), 'complete': True, 'pin_held': True,
                              'helper_share': {'jit': 0.25, 'gc': 0.3},
                              'stages': {s: figure(m) for s, m in
                                         (('biomes', 2.0), ('noise', 6.0), ('surface', 2.0))}}
                          for d in dims}},
          open(directory + '/vanilla.json', 'w'))
open(directory + '/vanilla.log', 'w').write('the vanilla half\n')
watch = options.get('watch', 'none')
if watch != 'none':
    mean, peak = (0.1, 0.3) if watch == 'quiet' else (2.5, 6.0)
    for half in ('stratum', 'vanilla'):
        json.dump({'available': True, 'wall_s': 600.0, 'own_cpu_s': 600.0,
                   'other_cpu_s': mean * 600, 'mean_other_cores': mean,
                   'peak_other_cores': peak, 'window_s': 60.0, 'samples': 121,
                   'quiet': watch == 'quiet', 'load_before': '9.00 9.00 9.00',
                   'load_after': '9.50 9.00 9.00'}, open(f'{directory}/{half}.watch.json', 'w'))
PY
}
summary_has() {  # summary_has <out dir> <text> <what>
    grep -qF -- "$2" "$1/summary.txt" || { echo "FAIL: $3"; cat "$1/summary.txt"; exit 1; }
    echo "ok: $3"
}

mkdir -p "${work}/earlier"
halves "${work}/earlier" watch=quiet
expect 0 "the baseline recomputes the comparison from an earlier run" \
    "Stratum / vanilla (mean, wall): 5.00x" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/earlier" --out "${work}/again"
summary_has "${work}/again" "between the minimum (100) and the best case (35 ms/chunk)" \
    "and judges it against the owner's bar"
summary_has "${work}/again" "seeds 0,42; the square of radius 1 around chunk (3, -4)" \
    "the header gives the seeds and square the halves measured, not the defaults"
summary_has "${work}/again" "quiet: other processes kept at most 0.5 cores" \
    "halves watched quiet are quiet, whatever the load average said or says now"
summary_has "${work}/again" "vanilla checks: 2 recording(s), pin held, area complete, estimates 1.000x to 1.000x" \
    "the vanilla half's own checks are in the row"
summary_has "${work}/again" "JIT 25%, GC 30%" "and its helpers' share of the worker's CPU"
[[ -f "${work}/again/vanilla.log" && -f "${work}/again/vanilla.watch.json" ]] \
    || { echo "FAIL: --from left the vanilla log or watch record behind"; ls "${work}/again"; exit 1; }
echo "ok: --from brings each half's log and watch record along"
check "${work}/again/baseline.json" "r['provisional'] is False and r['dimensions']['overworld']['measurement'] and r['seeds'] == [0, 42] and r['area'] == [3, -4, 1]" \
    "baseline.json says the same"

mkdir -p "${work}/unwatched"
halves "${work}/unwatched"
expect 0 "halves nobody watched" "PROVISIONAL" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/unwatched" --out "${work}/unwatched-out"
summary_has "${work}/unwatched-out" "stratum: not watched (its run kept no watch record)" \
    "make the run provisional, and say so"

mkdir -p "${work}/busy"
halves "${work}/busy" watch=busy
expect 0 "halves watched on a busy machine" "PROVISIONAL" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/busy" --out "${work}/busy-out"
summary_has "${work}/busy-out" "other processes kept 2.50 cores busy on average and 6.00 over the busiest minute" \
    "make it provisional with the figures that did"

mkdir -p "${work}/descheduled"
halves "${work}/descheduled" watch=quiet ratio=1.2
expect 3 "a recording whose estimates exceed its CPU is not a measurement" \
    "NOT A MEASUREMENT: vanilla seed 42: worker descheduled: wall overstates (the estimates are 1.200x" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/descheduled" --out "${work}/descheduled-out"
check "${work}/descheduled-out/baseline.json" "r['not_a_measurement'] == ['overworld'] and not r['dimensions']['overworld']['measurement'] and abs(r['dimensions']['overworld']['vanilla_checks']['estimate_cpu_ratio'] - 1.2) < 1e-9" \
    "the row and baseline.json say so, and the summary is still written"

mkdir -p "${work}/mismatched"
halves "${work}/mismatched" vanilla_area=3,-4,2
expect 1 "halves that measured different squares are refused" \
    "the reused halves measured different area" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/mismatched" --out "${work}/x"
expect 1 "and so is a half that is not what the command line asks for" \
    "the reused stratum half measured seeds [0, 42], not --seeds [0, 42, 7]" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/earlier" --seeds 0,42,7 \
    --out "${work}/x"
expect 1 "or a dimension a half does not have" "the reused stratum half has no end" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/earlier" --dimensions overworld,end \
    --out "${work}/x"
expect 1 "--from needs the half it stands in for" "has no stratum.json" \
    "${baseline}" --skip-stratum --skip-vanilla --from "${work}/again/nowhere"
