#!/usr/bin/env python3
# Stratum — what the vanilla server spends per chunk on the stages Stratum's
# chunk-cost bench times, with ONE worldgen worker thread.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
"""The Java baseline: the 1.21.11 server's own per-chunk stage times, one worker.

  tools/analysis/vanilla-chunk-cost.py run --accept-eula [--seeds a,b,...]
      [--dimensions overworld,amplified,large_biomes,end] [--area cx,cz,r]
      [--warmup-area cx,cz,r] [--threads n] [--structures on|off] [--heap 3G]
      [--jvm-arg=ARG ...] [--jdk DIR] [--work DIR] [--keep] [--json FILE]
  tools/analysis/vanilla-chunk-cost.py analyze RECORDING [--level ID] [--area cx,cz,r]
      [--json FILE]

WHAT IT RUNS. The server headless on a fresh world per seed and world preset
(`level-type` normal for the overworld and the End, amplified, large_biomes),
with `-Dmax.bg.threads=<n>` (default 1): the JVM property minecraft.wiki's
Java Edition 21w38a changelog documents as overriding the cap on the server's
background worker pool, the `Worker-Main-*` threads chunk generation runs on.
Per dimension it forceloads a warm-up area first (the JIT compiles what the
measured area then runs), waits until the workers go idle, starts the server's
own profiler (`/jfr start`), forceloads the measured area — by default the 121
chunks within 5 of chunk (-64, 64), the square stratum_chunk_cost_bench's
default spiral covers — waits for idle again, and stops it (`/jfr stop`).
Ticks are frozen first (`tick freeze`, as tools/fetch-vanilla does), so the
worker does generation and not a ticking world's fluids. Structures are off
by default (`generate-structures=false`): Stratum v1 generates none, and the
noise stage's structure terrain adaptation is work Stratum does not do.

WHAT IT READS. The recording's `minecraft.ChunkGeneration` events — the
server's documented "time taken to generate individual chunk stages"
(minecraft.wiki, Commands/jfr) — through the JDK's `jfr print --json`. Each
names its chunk, level, status and the thread it was committed on. An event's
own duration is NOT the stage's cost: it starts when the stage is scheduled,
and with one worker most stages queue behind others (measured on this server:
37 s of event time inside 3.4 s of wall time on one thread). So each event's
cost is taken as the worker's time between the previous completion and its
own: end - max(start, previous end), over the worker's timeline — with one
worker allowed, every Worker-Main thread's events together (PROVING THE PIN
says why). That is exact when the worker runs one task at a time and every
task ends in an event; work no event records (lighting, I/O callbacks) lands
on the next event to complete. The check is the workers' own CPU time over
the recording (jstack before and after), printed beside the sum of the
estimates: on the 25-chunk trial that settled the method they were 4.15 s
and 4.16 s.

THE CHECK, ENFORCED. The estimates are wall time. A worker that was runnable
but not running — descheduled, on a loaded machine — is charged for it, so
its estimates sum to more than its CPU: more than 5% more flags the recording
"worker descheduled: wall overstates", and its comparable figure is printed
again scaled to the CPU (each chunk's estimate times the recording's CPU over
its estimates — a correction, not a measurement). More than 5% less flags
"the estimates miss worker CPU": work no event ended. Parallelism under 0.95
alone is not a flag: idle time between tasks is in no estimate, so it moves
the parallelism and not the figures; the CPU-to-estimate ratio is what shows
whether they hold. A flagged recording, an incomplete area or a broken pin
makes `run` exit 3 and is named in the JSON (`flags`).

WHAT IT REPORTS. Per stage: median, p10, p90 and mean ms/chunk over the
measured area's chunks, and chunks/s as 1000 / mean. The stages comparable to
Stratum's are biomes, noise (density, aquifers, ore veins) and surface; their
per-chunk sum is "comparable". Every other stage the server runs —
structure_starts, structure_references, carvers, features, initialize_light,
light, spawn, full — is reported separately and is in no sum: Stratum v1
generates no structures, carvers or features, and PocketMine-MP lights its own
chunks. `full` runs on the server thread, among everything else it does, so
only its raw durations are given.

PROVING THE PIN. jstack before and after the recording gives every
`Worker-Main-*` thread's name and CPU; the recording says which threads
committed generation. The pin holds when generation was committed on
Worker-Main threads alone (`full` aside) and those threads together spent no
more CPU than one thread could over the time generation took: parallelism,
their CPU over the span of their events, at most 1.05. Unpinned, a 12-thread
box runs eleven workers, Worker-Main-1 to -11, all busy. Pinned, it runs one
and, at times, a fork-join spare or two (Worker-Main-2, -3) that the pool
starts while its worker blocks on a join, to run in its place: one at a
time, which is why the estimate reads them as one timeline. On the first
121-chunk overworld a spare committed 7 of 4362 generation events.

WHAT RAN BESIDE IT. One worldgen worker is not one thread of CPU: the JVM's
own helpers run on other cores beside it — its JIT compilers (`C1/C2
CompilerThread*`) and its garbage collector (`GC Thread#*`, `G1 *`, `VM
Thread`) — as do the `Server thread` (it schedules generation and runs
`full`), the `IO-Worker-*` threads and JFR's own. The same two jstacks give
each kind's CPU over the recording, printed beside the worker's and as a
share of it. A thread that ends inside the recording (the JVM retires
compiler threads it no longer needs) is in no second jstack; the last look
/proc had at it, from the half-second polls that wait for the workers to go
idle, is counted in its kind and also shown as "ended". Whatever is left of
the process's total CPU (/proc) is "unlisted": threads that came and went
between two looks. The recording's garbage collections are counted by
cause (`jdk.GarbageCollection`): the recording's own start and stop can
trigger heap-inspection collections, which the GC figure includes. Stratum's
bench is one thread with no such helpers. For a sensitivity run, pass the
JVM options through: --jvm-arg=-XX:ParallelGCThreads=1
--jvm-arg=-XX:ConcGCThreads=1 (written `--jvm-arg=...`, one option each).

HOW BUSY THE MACHINE WAS. Over each recording's generation, the machine's
busy CPU (/proc/stat) less the server's and this script's own, per second of
wall time: `other_cpu_cores`, the cores other processes kept busy while the
worker generated. The load average is printed too, but it counts the
server's own threads.

`analyze` is the reading alone, on a saved recording or on `jfr print --json`
output; tests/tools/vanilla_chunk_cost_test.sh drives it on synthetic events.

Needs a JDK (jfr and jstack beside java): --jdk, JAVA_HOME or PATH. Running
the server needs --accept-eula. Nothing this writes is committed: the worlds
and recordings are Mojang-derived (SPEC §12). They live in a fresh directory
made for the run — inside --work when given, else in the system's temporary
directory — and only that directory is removed afterwards, and only when the
run succeeded and --keep was not given; a failed run keeps it and names it,
so its server.log can be read. Only the timing summary is printed and, with
--json, written.
"""
import argparse
import datetime
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

VERSION = '1.21.11'
REPO = pathlib.Path(__file__).resolve().parent.parent.parent
COMPARABLE = ('biomes', 'noise', 'surface')
OTHER = ('structure_starts', 'structure_references', 'carvers', 'features', 'initialize_light',
         'light', 'spawn', 'full')
# The dimension a measurement names, the world preset that holds it, and its level.
DIMENSIONS = {
    'overworld': ('minecraft:normal', 'minecraft:overworld'),
    'amplified': ('minecraft:amplified', 'minecraft:overworld'),
    'large_biomes': ('minecraft:large_biomes', 'minecraft:overworld'),
    'end': ('minecraft:normal', 'minecraft:the_end'),
}
DEFAULT_SEEDS = '0,42,-4172144997902289642'
WORKER_PREFIX = 'Worker-Main'
EVENT = 'minecraft.ChunkGeneration'
GC_EVENT = 'jdk.GarbageCollection'
# How far the estimates' sum may sit from the workers' CPU, either way, before
# a recording's figures are flagged.
ESTIMATE_SLACK = 0.05
# The kinds of JVM thread whose CPU is reported beside the worker's, by name.
# /proc cuts a thread's name to 15 characters, so these match its prefix.
HELPER_KINDS = (
    ('jit', re.compile(r'C\d CompilerThre')),
    ('gc', re.compile(r'GC Thread|G1 |VM Thread$')),
    ('server', re.compile(r'Server thread$')),
    ('io', re.compile(r'IO-Worker')),
    ('jfr', re.compile(r'JFR ')),
)


def die(message):
    print(f'error: {message}', file=sys.stderr)
    sys.exit(1)


def log(message):
    print(f'==> {message}', file=sys.stderr, flush=True)


def load_average():
    try:
        with open('/proc/loadavg', encoding='ascii') as handle:
            return ' '.join(handle.read().split()[:3])
    except OSError:
        return 'unknown'


def parse_area(text):
    parts = text.split(',')
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(f'an area is cx,cz,r, not {text!r}')
    cx, cz, radius = (int(p) for p in parts)
    if radius < 0:
        raise argparse.ArgumentTypeError('an area radius is at least 0')
    return cx, cz, radius


def area_chunks(area):
    cx, cz, radius = area
    return {(x, z) for x in range(cx - radius, cx + radius + 1)
            for z in range(cz - radius, cz + radius + 1)}


# --- reading a recording ------------------------------------------------------

DURATION = re.compile(r'PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)(?:\.(\d+))?S)?')
INSTANT = re.compile(r'(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.(\d+))?(Z|[+-]\d\d:\d\d)?')


def nanos_of_duration(text):
    found = DURATION.fullmatch(text)
    if not found:
        raise ValueError(f'not an ISO-8601 duration: {text!r}')
    hours, minutes, seconds, fraction = found.groups()
    fraction = (fraction or '').ljust(9, '0')[:9]
    whole = int(hours or 0) * 3600 + int(minutes or 0) * 60 + int(seconds or 0)
    return whole * 1_000_000_000 + int(fraction or 0)


def nanos_of_instant(text):
    found = INSTANT.fullmatch(text)
    if not found:
        raise ValueError(f'not an ISO-8601 instant: {text!r}')
    base, fraction, zone = found.groups()
    zone = '+00:00' if zone in (None, 'Z') else zone
    seconds = int(datetime.datetime.fromisoformat(base + zone).timestamp())
    return seconds * 1_000_000_000 + int((fraction or '').ljust(9, '0')[:9] or 0)


def read_recording(path, jfr=None):
    """A .jfr recording's chunk and GC events (through `jfr print`), or
    `jfr print --json` output, as the document `jfr print --json` gives."""
    path = pathlib.Path(path)
    if path.suffix == '.jfr':
        if not jfr:
            die(f'reading {path} needs the JDK\'s jfr tool (--jdk)')
        out = subprocess.run([jfr, 'print', '--json', '--events', f'{EVENT},{GC_EVENT}',
                              str(path)], capture_output=True, text=True, check=False)
        if out.returncode != 0:
            die(f'jfr print failed on {path}: {out.stderr.strip()}')
        return json.loads(out.stdout)
    with open(path, encoding='utf-8') as handle:
        return json.load(handle)


def collections_of(document):
    """The recording's garbage collections, counted and their pauses summed by cause."""
    causes = {}
    for event in document['recording']['events']:
        if event.get('type') != GC_EVENT:
            continue
        values = event['values']
        cause = causes.setdefault(values.get('cause') or '?', {'count': 0, 'pause_ms': 0.0,
                                                               'collectors': []})
        cause['count'] += 1
        pauses = values.get('sumOfPauses')
        if pauses:
            cause['pause_ms'] += nanos_of_duration(pauses) / 1e6
        name = values.get('name') or '?'
        if name not in cause['collectors']:
            cause['collectors'].append(name)
    return causes


def read_events(path, jfr=None, document=None):
    """The ChunkGeneration events of a .jfr recording (through `jfr print`) or of
    `jfr print --json` output, as plain records."""
    if document is None:
        document = read_recording(path, jfr)
    events = []
    for event in document['recording']['events']:
        if event.get('type') != EVENT:
            continue
        values = event['values']
        start = nanos_of_instant(values['startTime'])
        duration = nanos_of_duration(values['duration'])
        thread = values.get('eventThread') or {}
        events.append({
            'status': values['status'].split(':')[-1],
            'level': values['level'],
            'chunk': (int(values['chunkPosX']), int(values['chunkPosZ'])),
            'thread': thread.get('javaName') or thread.get('osName') or '?',
            'start': start,
            'end': start + duration,
            'duration': duration,
        })
    return events


MERGED = WORKER_PREFIX + '-*'
# How far above the pin the workers' parallelism may read before the pin is
# called broken: jstack's CPU and JFR's clock are read at different moments.
PARALLELISM_SLACK = 0.05


def timeline_of(thread, allowed_workers):
    """The timeline an event's thread runs on. With one worker allowed, every
    Worker-Main thread is ONE timeline: the pool runs one task at a time, and
    the spare it starts while its worker blocks on a join (Worker-Main-2,
    -3) runs in that worker's place, not beside it."""
    if allowed_workers == 1 and thread.startswith(WORKER_PREFIX):
        return MERGED
    return thread


def exclusive_times(events, allowed_workers=1):
    """Each event's cost on its timeline: end - max(start, the previous end there).

    A stage's event starts when the stage is scheduled; on a single worker it
    then waits for whatever is queued ahead of it, so its duration is queueing
    plus work. Ordering a timeline's events by completion and charging each
    the time since the one before it completed takes the queueing out."""
    timelines = {}
    for event in events:
        timelines.setdefault(timeline_of(event['thread'], allowed_workers), []).append(event)
    for timeline_events in timelines.values():
        timeline_events.sort(key=lambda e: (e['end'], e['start']))
        previous = None
        for event in timeline_events:
            begin = event['start'] if previous is None else max(event['start'], previous)
            event['exclusive'] = event['end'] - begin
            previous = event['end']
    return timelines


def percentile(ordered, fraction):
    rank = fraction * (len(ordered) - 1)
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def summarise(milliseconds):
    if not milliseconds:
        return {'chunks': 0, 'available': False}
    ordered = sorted(milliseconds)
    mean = sum(ordered) / len(ordered)
    return {
        'chunks': len(ordered),
        'available': True,
        'median_ms': percentile(ordered, 0.5),
        'p10_ms': percentile(ordered, 0.1),
        'p90_ms': percentile(ordered, 0.9),
        'mean_ms': mean,
        'chunks_per_s': 1000.0 / mean if mean > 0 else 0.0,
    }


def analyse(events, level, area, allowed_workers=1):
    """Per-chunk stage costs for @p level's chunks inside @p area, from a
    recording whose server was allowed @p allowed_workers worldgen workers."""
    timelines = exclusive_times(events, allowed_workers)
    wanted = area_chunks(area)
    commits = {}
    for event in events:
        if event['status'] != 'full':
            commits[event['thread']] = commits.get(event['thread'], 0) + 1
    workers = sorted(t for t in commits if t.startswith(WORKER_PREFIX))
    strays = sorted(t for t in commits if not t.startswith(WORKER_PREFIX))

    per_chunk = {}
    for event in events:
        if event['level'] != level or event['chunk'] not in wanted:
            continue
        stages = per_chunk.setdefault(event['chunk'], {})
        # A chunk regenerated inside one recording (a discarded promotion) is
        # charged both times: that is what the worker spent on it.
        record = stages.setdefault(event['status'], {'exclusive': 0, 'duration': 0, 'count': 0})
        record['exclusive'] += event['exclusive']
        record['duration'] += event['duration']
        record['count'] += 1

    stages = {}
    for status in COMPARABLE + OTHER:
        present = [s[status] for s in per_chunk.values() if status in s]
        summary = summarise([r['exclusive'] / 1e6 for r in present])
        raw = summarise([r['duration'] / 1e6 for r in present])
        summary['raw_median_ms'] = raw.get('median_ms')
        summary['raw_mean_ms'] = raw.get('mean_ms')
        summary['repeated'] = sum(1 for r in present if r['count'] > 1)
        stages[status] = summary
    # full runs on the server thread among everything else it does: no
    # exclusive estimate means anything there, only its own durations.
    full = [s['full']['duration'] / 1e6 for s in per_chunk.values() if 'full' in s]
    stages['full'] = dict(summarise(full), raw_only=True)

    comparable = [sum(s[k]['exclusive'] for k in COMPARABLE) / 1e6
                  for s in per_chunk.values() if all(k in s for k in COMPARABLE)]
    missing = {status: len([c for c in wanted if status not in per_chunk.get(c, {})])
               for status in COMPARABLE + ('full',)}

    spans = {}
    for name, timeline_events in sorted(timelines.items()):
        spans[name] = {
            'events': len(timeline_events),
            'busy_s': sum(e['exclusive'] for e in timeline_events) / 1e9,
            'span_s': (max(e['end'] for e in timeline_events) -
                       min(e['start'] for e in timeline_events)) / 1e9,
        }
    worker_events = [e for e in events if e['thread'].startswith(WORKER_PREFIX)]
    return {
        'level': level,
        'area': {'centre': [area[0], area[1]], 'radius': area[2], 'chunks': len(wanted)},
        'stages': stages,
        'comparable': summarise(comparable),
        'missing': missing,
        'complete': not any(missing.values()),
        'allowed_workers': allowed_workers,
        'commits': commits,
        'worker_threads': workers,
        'stray_threads': strays,
        # Generation committed on Worker-Main threads and nowhere else. Half
        # the pin's evidence; run() adds the other half, the workers' CPU.
        'workers_only': bool(workers) and not strays,
        'timelines': spans,
        'worker_busy_s': sum(e['exclusive'] for e in worker_events) / 1e9,
        'worker_span_s': ((max(e['end'] for e in worker_events) -
                           min(e['start'] for e in worker_events)) / 1e9 if worker_events else 0.0),
        'samples': {
            'comparable_ms': comparable,
            **{status: [s[status]['exclusive'] / 1e6 for s in per_chunk.values() if status in s]
               for status in COMPARABLE},
        },
    }


def judge_pin(result, worker_cpu_s, allowed_workers):
    """The pin's other half, from each Worker-Main thread's CPU over the
    recording: however many of them exist, together they spent no more CPU
    than @p allowed_workers threads could over the time generation took.
    Unpinned, eleven workers on a 12-thread box spend several times that.

    Then the estimates against that CPU (THE CHECK, ENFORCED), and the
    recording's flags: everything that makes its figures not a measurement."""
    result['worker_cpu_s'] = worker_cpu_s
    result['jstack_workers'] = sorted(worker_cpu_s)
    result['worker_cpu_total_s'] = sum(worker_cpu_s.values())
    span = result['worker_span_s']
    cpu = result['worker_cpu_total_s']
    result['parallelism'] = cpu / span if span > 0 else 0.0
    result['pin_held'] = (result['workers_only'] and
                          result['parallelism'] <= allowed_workers + PARALLELISM_SLACK)
    busy = result['worker_busy_s']
    result['estimate_cpu_ratio'] = busy / cpu if cpu > 0 else None
    ratio = result['estimate_cpu_ratio']
    result['wall_overstates'] = ratio is None or ratio > 1 + ESTIMATE_SLACK
    result['estimate_misses_cpu'] = ratio is not None and ratio < 1 - ESTIMATE_SLACK
    # Each chunk's comparable estimate scaled by the workers' CPU over the
    # estimates: what the figures would read had the worker never been
    # descheduled, if being descheduled hit every stage alike.
    scale = cpu / busy if busy > 0 and cpu > 0 else None
    result['cpu_scale'] = scale
    result['samples']['comparable_cpu_scaled_ms'] = (
        [v * scale for v in result['samples']['comparable_ms']] if scale else [])
    result['comparable_cpu_scaled'] = summarise(result['samples']['comparable_cpu_scaled_ms'])
    result['flags'] = flags_of(result)
    return result


def flags_of(result):
    """Why a judged recording's figures are not a measurement, if they are not."""
    flags = []
    if not result['complete']:
        flags.append(f'incomplete: area chunks with no event {result["missing"]}')
    if not result['pin_held']:
        flags.append(f'the pin did not hold (parallelism {result["parallelism"]:.2f}, '
                     f'generation committed on {", ".join(sorted(result["commits"]))})')
    ratio = result['estimate_cpu_ratio']
    if ratio is None:
        flags.append('no worker CPU was measured, so the estimates are unchecked')
    elif result['wall_overstates']:
        flags.append(f'worker descheduled: wall overstates (the estimates are {ratio:.3f}x the '
                     'workers\' CPU)')
    elif result['estimate_misses_cpu']:
        flags.append(f'the estimates miss worker CPU (they are {ratio:.3f}x it): work no event '
                     'ended')
    if result.get('vanished_workers'):
        flags.append('a worker thread ended inside the recording, so its CPU is not counted: '
                     + ', '.join(result['vanished_workers']))
    return flags


def kind_of(name):
    if name.startswith(WORKER_PREFIX):
        return 'worker'
    for kind, pattern in HELPER_KINDS:
        if pattern.match(name):
            return kind
    return 'other'


def thread_kinds(before, after, process_s, ended=None):
    """CPU seconds over a recording by kind of JVM thread, from jstack before
    and after (@p before, @p after: native id -> (name, CPU ms)). A thread
    only in @p after started inside the recording. One that ended inside it
    is in no second jstack: @p ended (native id -> (name, CPU seconds)) is
    what /proc saw of each, counted in its kind and also as "ended". Whatever
    is left of @p process_s (the process's total over the recording) is
    "unlisted": threads that started and ended between two looks."""
    kinds = {kind: 0.0 for kind in ('worker',) + tuple(k for k, _ in HELPER_KINDS) + ('other',)}
    for nid, (name, _) in after.items():
        kinds[kind_of(name)] += thread_delta(before, after, nid)
    kinds['ended'] = 0.0
    for name, seconds in (ended or {}).values():
        kinds[kind_of(name)] += seconds
        kinds['ended'] += seconds
    listed = sum(v for k, v in kinds.items() if k != 'ended')
    kinds['unlisted'] = max(0.0, process_s - listed) if process_s is not None else None
    return kinds


def helper_shares(kinds):
    """Each helper kind's CPU as a share of the worker's."""
    worker = kinds.get('worker') or 0.0
    return {kind: (value / worker if worker > 0 and value is not None else None)
            for kind, value in kinds.items() if kind not in ('worker', 'ended')}


def pool(results):
    """Several runs' chunks as one sample, and their checks together."""
    out = {'runs': len(results), 'stages': {}, 'complete': all(r['complete'] for r in results),
           'pin_held': all(r.get('pin_held', r['workers_only']) for r in results)}
    for status in COMPARABLE:
        out['stages'][status] = summarise([v for r in results for v in r['samples'][status]])
    out['comparable'] = summarise([v for r in results for v in r['samples']['comparable_ms']])
    judged = [r for r in results if 'flags' in r]
    if not judged:
        return out
    out['comparable_cpu_scaled'] = summarise(
        [v for r in judged for v in r['samples']['comparable_cpu_scaled_ms']])
    busy = sum(r['worker_busy_s'] for r in judged)
    cpu = sum(r['worker_cpu_total_s'] for r in judged)
    ratios = [r['estimate_cpu_ratio'] for r in judged if r['estimate_cpu_ratio'] is not None]
    out['estimate_cpu_ratio'] = busy / cpu if cpu > 0 else None
    out['estimate_cpu_ratio_range'] = [min(ratios), max(ratios)] if ratios else None
    out['flags'] = [f'seed {r.get("seed", "?")}: {flag}' for r in judged for flag in r['flags']]
    kinds = {}
    for r in judged:
        for kind, value in r.get('thread_cpu_s', {}).items():
            if value is not None:
                kinds[kind] = kinds.get(kind, 0.0) + value
    out['thread_cpu_s'] = kinds
    out['helper_share'] = helper_shares(kinds)
    others = [r['other_cpu_cores'] for r in judged if r.get('other_cpu_cores') is not None]
    out['other_cpu_cores_max'] = max(others) if others else None
    return out


def describe(figure):
    if not figure.get('available'):
        return '-'
    return (f'median {figure["median_ms"]:8.2f}  p10 {figure["p10_ms"]:8.2f}  '
            f'p90 {figure["p90_ms"]:8.2f}  mean {figure["mean_ms"]:8.2f} ms/chunk  '
            f'{figure["chunks_per_s"]:7.2f} chunks/s')


def print_result(title, result):
    print(f'{title}:')
    for status in COMPARABLE:
        print(f'  {status:10s} {describe(result["stages"][status])}')
    print(f'  {"comparable":10s} {describe(result["comparable"])}')
    if 'missing' in result:
        for status in OTHER:
            figure = result['stages'][status]
            if figure.get('available'):
                tag = ' (raw: runs on the server thread)' if figure.get('raw_only') else ''
                print(f'  {status:20s} median {figure["median_ms"]:8.3f}  mean '
                      f'{figure["mean_ms"]:8.3f} ms/chunk over {figure["chunks"]}{tag}  [not '
                      'comparable]')
        if not result['complete']:
            print(f'  INCOMPLETE: area chunks with no event: {result["missing"]}')
        commits = ', '.join(f'{thread} ({count})' for thread, count in
                            sorted(result['commits'].items()))
        print(f'  generation committed on: {commits}'
              + ('' if result['workers_only'] else '  <-- NOT ONLY ON WORLDGEN WORKERS'))
        if 'pin_held' in result:
            ratio = result['estimate_cpu_ratio']
            print(f'  workers: {result["worker_cpu_total_s"]:.2f} s of CPU over '
                  f'{result["worker_span_s"]:.2f} s of generation, parallelism '
                  f'{result["parallelism"]:.2f} (the pin allows {result["allowed_workers"]}); '
                  f'the estimates sum to {result["worker_busy_s"]:.2f} s'
                  + (f', {ratio:.3f}x the CPU' if ratio is not None else '')
                  + ('' if result['pin_held'] else '  <-- THE PIN DID NOT HOLD'))
            if result['parallelism'] < 1 - PARALLELISM_SLACK and not result['wall_overstates']:
                print(f'  note: the workers were idle or descheduled for '
                      f'{1 - result["parallelism"]:.0%} of the span; the estimates match their '
                      'CPU, so the idle time is in no figure')
    if 'thread_cpu_s' in result:
        print_helpers(result['thread_cpu_s'])
    if result.get('collections'):
        print('  garbage collections: ' + '; '.join(
            f'{cause} x{c["count"]} ({c["pause_ms"]:.1f} ms paused, {"/".join(c["collectors"])})'
            for cause, c in sorted(result['collections'].items())))
    if result.get('other_cpu_cores') is not None:
        print(f'  other processes kept {result["other_cpu_cores"]:.2f} cores busy meanwhile')
    if result.get('other_cpu_cores_max') is not None:
        print(f'  other processes: at most {result["other_cpu_cores_max"]:.2f} cores busy over '
              'any recording')
    for flag in result.get('flags', []):
        print(f'  FLAGGED: {flag}')
    if result.get('flags') and result.get('comparable_cpu_scaled', {}).get('available'):
        print(f'  {"cpu-scaled":10s} {describe(result["comparable_cpu_scaled"])}  [a correction, '
              'not a measurement]')
    sys.stdout.flush()


def print_helpers(kinds):
    worker = kinds.get('worker') or 0.0
    names = {'jit': 'JIT', 'gc': 'GC', 'server': 'Server thread', 'io': 'IO-Worker',
             'jfr': 'JFR', 'other': 'other', 'unlisted': 'unlisted'}

    def part(kind):
        value = kinds.get(kind)
        if value is None:
            return f'{names[kind]} -'
        share = f' ({value / worker:.0%})' if worker > 0 else ''
        return f'{names[kind]} {value:.2f} s{share}'
    print(f'  beside the worker\'s {worker:.2f} s of CPU, on other cores: '
          + ', '.join(part(kind) for kind in names)
          + (f' (of these, {kinds["ended"]:.2f} s on threads that ended inside the recording)'
             if kinds.get('ended') else ''))


# --- running the server -------------------------------------------------------

def find_jdk(option):
    """java, jfr and jstack from one JDK."""
    candidates = []
    if option:
        home = pathlib.Path(option)
        candidates.append(home / 'bin' if (home / 'bin').is_dir() else home)
    if os.environ.get('JAVA_HOME'):
        candidates.append(pathlib.Path(os.environ['JAVA_HOME']) / 'bin')
    if shutil.which('java'):
        candidates.append(pathlib.Path(os.path.realpath(shutil.which('java'))).parent)
    for directory in candidates:
        tools = {name: directory / name for name in ('java', 'jfr', 'jstack')}
        if all(path.is_file() and os.access(path, os.X_OK) for path in tools.values()):
            return {name: str(path) for name, path in tools.items()}
    tried = ', '.join(str(c) for c in candidates) or 'nothing'
    die(f'no JDK with java, jfr and jstack found (tried {tried}); a JRE has no jfr or jstack. '
        'Pass --jdk <JDK home>')
    return None


def find_jar():
    fixtures = pathlib.Path(os.environ.get('STRATUM_FIXTURES_DIR') or REPO / '.fixtures')
    jars = sorted((fixtures / VERSION).glob('server*.jar'))
    if not jars:
        die(f'no server jar under {fixtures / VERSION}; run tools/fetch-vanilla first')
    return jars[0].resolve()


JSTACK_THREAD = re.compile(r'"([^"]+)".*?\bcpu=([\d.]+)ms(?:.*?\bnid=(\S+))?')


def parse_jstack(text):
    """Every thread a jstack lists: native id (decimal, as /proc names it) ->
    (name, CPU milliseconds). Keyed by native id so a thread that ended and
    one that took its name apart are two threads."""
    threads = {}
    for line in text.splitlines():
        found = JSTACK_THREAD.match(line)
        if found:
            nid = found.group(3)
            if nid and nid.startswith('0x'):
                nid = str(int(nid, 16))
            threads[nid or found.group(1)] = (found.group(1), float(found.group(2)))
    return threads


def thread_cpu(jstack, pid):
    """parse_jstack of a jstack of @p pid."""
    out = subprocess.run([jstack, str(pid)], capture_output=True, text=True, check=False)
    if out.returncode != 0:
        die(f'jstack {pid} failed: {out.stderr.strip()}')
    return parse_jstack(out.stdout)


def thread_delta(before, after, nid):
    """Thread @p nid's CPU seconds between two parse_jstack()s; one that
    started in between started from nothing."""
    name, ms = after[nid]
    earlier = before.get(nid)
    start = earlier[1] if earlier and earlier[0] == name else 0.0
    return max(0.0, ms - start) / 1000.0


def worker_deltas(before, after):
    """Each Worker-Main thread's CPU seconds between two parse_jstack()s, by name."""
    out = {}
    for nid, (name, _) in sorted(after.items(), key=lambda item: item[1][0]):
        if name.startswith(WORKER_PREFIX):
            out[name] = out.get(name, 0.0) + thread_delta(before, after, nid)
    return out


def stat_fields(path):
    """/proc/<pid>/stat's fields from the state on (field 3), or None."""
    try:
        stat = pathlib.Path(path).read_text(encoding='ascii', errors='replace')
    except OSError:
        return None
    return stat[stat.rindex(')') + 2:].split()


def process_ticks(pid, children=False):
    """User+system clock ticks of every thread of @p pid, ended ones included;
    with @p children, its waited-for children's as well."""
    fields = stat_fields(f'/proc/{pid}/stat')
    if fields is None:
        return 0
    return sum(int(v) for v in fields[11:15 if children else 13])


def busy_ticks():
    """The machine's busy clock ticks, every CPU: /proc/stat's user, nice,
    system, irq, softirq and steal."""
    try:
        with open('/proc/stat', encoding='ascii') as handle:
            fields = handle.readline().split()
    except OSError:
        return None
    user, nice, system, _idle, _iowait, irq, softirq, steal = (int(v) for v in fields[1:9])
    return user + nice + system + irq + softirq + steal


def task_ticks(pid):
    """Every live thread of @p pid: native id -> (name, user+system clock ticks)."""
    threads = {}
    try:
        tasks = list(pathlib.Path(f'/proc/{pid}/task').iterdir())
    except OSError:
        return threads
    for task in tasks:
        try:
            stat = (task / 'stat').read_text(encoding='ascii', errors='replace')
        except OSError:
            continue
        fields = stat[stat.rindex(')') + 2:].split()
        threads[task.name] = (stat[stat.index('(') + 1:stat.rindex(')')],
                              int(fields[11]) + int(fields[12]))
    return threads


def worker_ticks(threads):
    """Summed clock ticks of the Worker-Main threads of a task_ticks()."""
    return sum(ticks for name, ticks in threads.values() if name.startswith(WORKER_PREFIX))


class OtherCpu:
    """The machine's busy CPU over an interval less @p pid's and this
    script's own (with its waited-for children): what other processes ran."""

    def __init__(self, pid):
        self.pid = pid
        self.start = self.read()

    def read(self):
        return (time.monotonic(), busy_ticks(), process_ticks(self.pid),
                process_ticks(os.getpid(), children=True))

    def cores(self):
        """Cores other processes kept busy since construction, or None."""
        end = self.read()
        if self.start[1] is None or end[1] is None or end[0] <= self.start[0]:
            return None
        ticks = (end[1] - self.start[1]) - (end[2] - self.start[2]) - (end[3] - self.start[3])
        return max(0.0, ticks / os.sysconf('SC_CLK_TCK') / (end[0] - self.start[0]))


class Server:
    def __init__(self, directory, jdk, jar, seed, preset, threads, structures, heap, jvm_args):
        self.directory = directory
        self.jdk = jdk
        directory.mkdir(parents=True)
        # The EULA is accepted by whoever passed --accept-eula; this records it.
        (directory / 'eula.txt').write_text('eula=true\n', encoding='ascii')
        # server-port=0: any free port, nothing connects. max-tick-time=-1: no
        # watchdog (tools/fetch-vanilla says why).
        (directory / 'server.properties').write_text('\n'.join([
            f'level-seed={seed}', 'level-name=world', f'level-type={preset}',
            'online-mode=false', 'server-port=0', 'max-players=1', 'spawn-protection=0',
            'view-distance=10', 'simulation-distance=10',
            f'generate-structures={"true" if structures else "false"}',
            'spawn-monsters=false', 'spawn-npcs=false', 'spawn-animals=false',
            'sync-chunk-writes=false', 'enable-command-block=false', 'max-tick-time=-1',
            'motd=stratum vanilla chunk cost', '']), encoding='ascii')
        self.log_path = directory / 'server.log'
        self.seen = {}
        self.log_handle = open(self.log_path, 'w', encoding='utf-8')
        self.command = [jdk['java'], f'-Xmx{heap}', f'-Dmax.bg.threads={threads}', *jvm_args,
                        '-jar', str(jar), '--nogui']
        self.process = subprocess.Popen(self.command, cwd=directory, stdin=subprocess.PIPE,
                                        stdout=self.log_handle, stderr=subprocess.STDOUT,
                                        text=True)

    def log_text(self):
        return self.log_path.read_text(encoding='utf-8', errors='replace')

    def wait_for(self, pattern, timeout, after=0):
        """The first match of @p pattern in the log past offset @p after."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = re.search(pattern, self.log_text()[after:])
            if found:
                return found
            if self.process.poll() is not None:
                die(f'the server exited (see {self.log_path}):\n' + self.log_text()[-3000:])
            time.sleep(0.5)
        die(f'no "{pattern}" from the server within {timeout}s (see {self.log_path})')
        return None

    def send(self, command):
        if self.process.poll() is not None:
            die(f'the server exited (see {self.log_path})')
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def threads(self):
        """task_ticks() now, each thread also kept in self.seen: the last look
        at a thread that ends before the next jstack is the only one."""
        now = task_ticks(self.process.pid)
        self.seen.update(now)
        return now

    def settle(self, quiet=3.0, timeout=1800.0):
        """Waits until the Worker-Main threads have spent no CPU for @p quiet seconds."""
        start = time.monotonic()
        last = worker_ticks(self.threads())
        still_since = time.monotonic()
        while time.monotonic() - start < timeout:
            time.sleep(0.5)
            if self.process.poll() is not None:
                die(f'the server exited (see {self.log_path})')
            now = worker_ticks(self.threads())
            if now != last:
                last = now
                still_since = time.monotonic()
            elif time.monotonic() - still_since >= quiet and time.monotonic() - start >= 2.0:
                return time.monotonic() - start
        die(f'the workers did not go idle within {timeout}s')
        return None

    def stop(self):
        if self.process.poll() is None:
            try:
                self.send('stop')
                self.process.wait(timeout=180)
            except (subprocess.TimeoutExpired, BrokenPipeError, OSError):
                self.process.kill()
                self.process.wait()
        self.log_handle.close()


def forceload(server, level, area):
    """/forceload takes block coordinates and at most 256 chunks a command."""
    cx, cz, radius = area
    for z0 in range(cz - radius, cz + radius + 1, 16):
        for x0 in range(cx - radius, cx + radius + 1, 16):
            x1 = min(x0 + 15, cx + radius)
            z1 = min(z0 + 15, cz + radius)
            server.send(f'execute in {level} run forceload add {x0 * 16} {z0 * 16} '
                        f'{x1 * 16 + 15} {z1 * 16 + 15}')


def measure(server, jdk, name, level, area, warmup_area, threads):
    log(f'{name}: warming up on the {2 * warmup_area[2] + 1}-chunk square around '
        f'({warmup_area[0]}, {warmup_area[1]})')
    forceload(server, level, warmup_area)
    server.settle()
    mark = len(server.log_text())
    before = thread_cpu(jdk['jstack'], server.process.pid)
    process_before = process_ticks(server.process.pid)
    server.seen = {}
    first = server.threads()
    server.send('jfr start')
    server.wait_for(r'JFR profiling started', 60, mark)
    load_before = load_average()
    log(f'{name}: generating the {2 * area[2] + 1}-chunk square around ({area[0]}, {area[1]})')
    began = time.monotonic()
    others = OtherCpu(server.process.pid)
    forceload(server, level, area)
    settled = server.settle()
    other_cores = others.cores()
    mark = len(server.log_text())
    server.send('jfr stop')
    found = server.wait_for(r'JFR profiling stopped and dumped to (\S+\.jfr)', 300, mark)
    elapsed = time.monotonic() - began
    load_after = load_average()
    server.threads()
    after = thread_cpu(jdk['jstack'], server.process.pid)
    process_s = (process_ticks(server.process.pid) - process_before) / os.sysconf('SC_CLK_TCK')
    # Threads /proc saw inside the recording that the second jstack no longer
    # lists: what each spent between the first look and the last.
    hertz = os.sysconf('SC_CLK_TCK')
    ended = {tid: (thread, (ticks - first[tid][1] if tid in first else ticks) / hertz)
             for tid, (thread, ticks) in server.seen.items() if tid not in after}
    recording = server.directory / found.group(1)
    document = read_recording(recording, jdk['jfr'])
    result = analyse(read_events(recording, document=document), level, area, threads)
    result['collections'] = collections_of(document)
    result['dimension'] = name
    result['wall_s'] = elapsed
    result['settled_after_s'] = settled
    result['load_average_before'] = load_before
    result['load_average_after'] = load_after
    result['other_cpu_cores'] = other_cores
    result['thread_cpu_s'] = thread_kinds(before, after, process_s, ended)
    result['ended_threads'] = sorted({thread for thread, _ in ended.values()})
    result['helper_share'] = helper_shares(result['thread_cpu_s'])
    result['vanished_workers'] = sorted(thread for nid, (thread, _) in before.items()
                                        if thread.startswith(WORKER_PREFIX) and nid not in after)
    judge_pin(result, worker_deltas(before, after), threads)
    return result


def run(args):
    if not args.accept_eula:
        die('this runs the Minecraft server, so it needs --accept-eula')
    if not pathlib.Path('/proc/self/task').is_dir():
        die('run reads worker CPU from /proc, so it needs Linux (analyze runs anywhere)')
    jdk = find_jdk(args.jdk)
    jar = find_jar()
    seeds = [int(s) for s in args.seeds.split(',')]
    names = args.dimensions.split(',')
    for name in names:
        if name not in DIMENSIONS:
            die(f'unknown dimension {name!r} ({", ".join(DIMENSIONS)})')
    java_version = (subprocess.run([jdk['java'], '-version'], capture_output=True, text=True,
                                   check=False).stderr.splitlines() or ['unknown'])[0]
    # A fresh directory of the run's own, inside --work when given: only it is
    # ever removed, never --work or anything already there.
    if args.work:
        pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix='stratum-vanilla-cost-', dir=args.work))
    report = {'tool': 'vanilla-chunk-cost', 'minecraft_version': VERSION, 'jar': jar.name,
              'java': java_version, 'threads': args.threads, 'structures': args.structures,
              'heap': args.heap, 'jvm_args': args.jvm_arg, 'area': list(args.area),
              'warmup_area': list(args.warmup_area), 'seeds': seeds,
              'load_average_start': load_average(), 'runs': []}
    log(f'{java_version}; -Dmax.bg.threads={args.threads}'
        + (f' {" ".join(args.jvm_arg)}' if args.jvm_arg else '') + f'; work in {work}')
    # One server per seed and preset: the overworld and the End share the
    # normal preset's.
    presets = []
    for name in names:
        if DIMENSIONS[name][0] not in presets:
            presets.append(DIMENSIONS[name][0])
    succeeded = False
    try:
        for seed in seeds:
            for preset in presets:
                directory = work / f'{preset.split(":")[1]}-seed{seed}'
                log(f'seed {seed}: starting the server ({preset})')
                server = Server(directory, jdk, jar, seed, preset, args.threads,
                                args.structures == 'on', args.heap, args.jvm_arg)
                try:
                    server.wait_for(r'Done \(', 600)
                    mark = len(server.log_text())
                    server.send('tick freeze')
                    server.wait_for(r'(?i)frozen', 60, mark)
                    for name in names:
                        if DIMENSIONS[name][0] != preset:
                            continue
                        result = measure(server, jdk, name, DIMENSIONS[name][1], args.area,
                                         args.warmup_area, args.threads)
                        result['seed'] = seed
                        result['preset'] = preset
                        report['runs'].append(result)
                        print_result(f'{name} seed {seed} (load {result["load_average_before"]} '
                                     f'-> {result["load_average_after"]})', result)
                finally:
                    server.stop()
        succeeded = True
    finally:
        if succeeded and not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            # Its server logs are what a failure is read from. The worlds and
            # recordings in it are Mojang-derived: never commit them.
            log(f'kept {work}' + ('' if succeeded else ' (the run failed; each server\'s log is '
                                  'server.log in its directory there)')
                + ': Mojang-derived, never commit it; remove it when done')
    report['dimensions'] = {}
    for name in names:
        results = [r for r in report['runs'] if r['dimension'] == name]
        report['dimensions'][name] = pool(results)
        print_result(f'{name}, every seed, {len(results)} run(s)', report['dimensions'][name])
    report['load_average_end'] = load_average()
    print(f'load average at end: {report["load_average_end"]}')
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    flagged = [f'{r["dimension"]} seed {r["seed"]}: {flag}' for r in report['runs']
               for flag in r['flags']]
    if flagged:
        log('WARNING: these recordings are not a measurement:\n  ' + '\n  '.join(flagged))
        return 3
    return 0


def analyze(args):
    jfr = None
    if pathlib.Path(args.recording).suffix == '.jfr':
        jfr = find_jdk(args.jdk)['jfr']
    document = read_recording(args.recording, jfr)
    result = analyse(read_events(args.recording, document=document), args.level, args.area,
                     args.threads)
    result['collections'] = collections_of(document)
    print_result(f'{args.level} around ({args.area[0]}, {args.area[1]}), radius {args.area[2]}',
                 result)
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return 0


def main():
    parser = argparse.ArgumentParser(prog='tools/analysis/vanilla-chunk-cost.py',
                                     description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    run_parser = sub.add_parser('run', help='run the server and measure')
    run_parser.add_argument('--accept-eula', action='store_true')
    run_parser.add_argument('--seeds', default=DEFAULT_SEEDS)
    run_parser.add_argument('--dimensions', default=','.join(DIMENSIONS))
    run_parser.add_argument('--area', type=parse_area, default=(-64, 64, 5))
    run_parser.add_argument('--warmup-area', type=parse_area, default=(64, -64, 3))
    run_parser.add_argument('--threads', type=int, default=1)
    run_parser.add_argument('--structures', choices=('on', 'off'), default='off')
    run_parser.add_argument('--heap', default='3G')
    run_parser.add_argument('--jvm-arg', action='append', default=[])
    run_parser.add_argument('--jdk')
    run_parser.add_argument('--work')
    run_parser.add_argument('--keep', action='store_true')
    run_parser.add_argument('--json')
    analyze_parser = sub.add_parser('analyze', help='read a saved recording')
    analyze_parser.add_argument('recording')
    analyze_parser.add_argument('--level', default='minecraft:overworld')
    analyze_parser.add_argument('--area', type=parse_area, default=(-64, 64, 5))
    analyze_parser.add_argument('--threads', type=int, default=1)
    analyze_parser.add_argument('--jdk')
    analyze_parser.add_argument('--json')
    args = parser.parse_args()
    if args.command == 'run':
        if args.threads < 1:
            die('--threads is at least 1')
        return run(args)
    return analyze(args)


if __name__ == '__main__':
    sys.exit(main())
