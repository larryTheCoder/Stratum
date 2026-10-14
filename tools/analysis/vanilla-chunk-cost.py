#!/usr/bin/env python3
# Stratum — what the vanilla server spends per chunk on the stages Stratum's
# chunk-cost bench times, with ONE worldgen worker thread.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
"""The Java baseline: the 1.21.11 server's own per-chunk stage times, one worker.

  tools/analysis/vanilla-chunk-cost.py run --accept-eula [--seeds a,b,...]
      [--dimensions overworld,amplified,large_biomes,end] [--area cx,cz,r]
      [--warmup-area cx,cz,r] [--threads n] [--structures on|off] [--heap 3G]
      [--jdk DIR] [--work DIR] [--keep] [--json FILE]
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
cost is taken as the worker's time between the previous completion on that
thread and its own: end - max(start, previous end). That is exact when the
worker runs one task at a time and every task ends in an event; work no event
records (lighting, I/O callbacks) lands on the next event to complete. The
check is the worker's own CPU time over the recording (jstack before and
after), printed beside the sum of the estimates: on the 25-chunk trial that
settled the method they were 4.15 s and 4.16 s.

WHAT IT REPORTS. Per stage: median, p10, p90 and mean ms/chunk over the
measured area's chunks, and chunks/s as 1000 / mean. The stages comparable to
Stratum's are biomes, noise (density, aquifers, ore veins) and surface; their
per-chunk sum is "comparable". Every other stage the server runs —
structure_starts, structure_references, carvers, features, initialize_light,
light, spawn, full — is reported separately and is in no sum: Stratum v1
generates no structures, carvers or features, and PocketMine-MP lights its own
chunks. `full` runs on the server thread, among everything else it does, so
only its raw durations are given.

PROVING THE PIN. jstack after the measurement lists every `Worker-Main-*`
thread and the CPU each spent over the recording; the recording says which
threads committed generation events. The pin holds when one worker committed
every event but `full`'s. (On a 12-thread box the property's absence shows
eleven workers, Worker-Main-1 to -11, all busy. With it, a second worker can
exist — a fork-join pool may add a spare while one blocks — and holds none of
the generation; the report says if that ever changes.)

`analyze` is the reading alone, on a saved recording or on `jfr print --json`
output; tests/tools/vanilla_chunk_cost_test.sh drives it on synthetic events.

Needs a JDK (jfr and jstack beside java): --jdk, JAVA_HOME or PATH. Running
the server needs --accept-eula. Nothing this writes is committed: the worlds
and recordings are Mojang-derived (SPEC §12) and live in --work (a temporary
directory, removed afterwards, unless --keep); only the timing summary is
printed and, with --json, written.
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


def read_events(path, jfr=None):
    """The ChunkGeneration events of a .jfr recording (through `jfr print`) or of
    `jfr print --json` output, as plain records."""
    path = pathlib.Path(path)
    if path.suffix == '.jfr':
        if not jfr:
            die(f'reading {path} needs the JDK\'s jfr tool (--jdk)')
        out = subprocess.run([jfr, 'print', '--json', '--events', EVENT, str(path)],
                             capture_output=True, text=True, check=False)
        if out.returncode != 0:
            die(f'jfr print failed on {path}: {out.stderr.strip()}')
        document = json.loads(out.stdout)
    else:
        with open(path, encoding='utf-8') as handle:
            document = json.load(handle)
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


def exclusive_times(events):
    """Each event's cost on its thread: end - max(start, the previous end there).

    A stage's event starts when the stage is scheduled; on a single worker it
    then waits for whatever is queued ahead of it, so its duration is queueing
    plus work. Ordering a thread's events by completion and charging each the
    time since the one before it completed takes the queueing out."""
    by_thread = {}
    for event in events:
        by_thread.setdefault(event['thread'], []).append(event)
    for thread_events in by_thread.values():
        thread_events.sort(key=lambda e: (e['end'], e['start']))
        previous = None
        for event in thread_events:
            begin = event['start'] if previous is None else max(event['start'], previous)
            event['exclusive'] = event['end'] - begin
            previous = event['end']
    return by_thread


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
    by_thread = exclusive_times(events)
    wanted = area_chunks(area)
    generation_threads = sorted({e['thread'] for e in events if e['status'] != 'full'})
    workers = [t for t in generation_threads if t.startswith(WORKER_PREFIX)]
    strays = [t for t in generation_threads if not t.startswith(WORKER_PREFIX)]

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

    threads = {}
    for thread, thread_events in sorted(by_thread.items()):
        threads[thread] = {
            'events': len(thread_events),
            'busy_s': sum(e['exclusive'] for e in thread_events) / 1e9,
            'span_s': (max(e['end'] for e in thread_events) -
                       min(e['start'] for e in thread_events)) / 1e9,
        }
    return {
        'level': level,
        'area': {'centre': [area[0], area[1]], 'radius': area[2], 'chunks': len(wanted)},
        'stages': stages,
        'comparable': summarise(comparable),
        'missing': missing,
        'complete': not any(missing.values()),
        'generation_threads': generation_threads,
        'worker_threads': workers,
        'stray_threads': strays,
        # The pin holds when no more workers than it allows committed
        # generation, and nothing but a worker did.
        'pin_held': 0 < len(workers) <= allowed_workers and not strays,
        'threads': threads,
        'samples': {
            'comparable_ms': comparable,
            **{status: [s[status]['exclusive'] / 1e6 for s in per_chunk.values() if status in s]
               for status in COMPARABLE},
        },
    }


def pool(results):
    """Several runs' chunks as one sample."""
    out = {'runs': len(results), 'stages': {}, 'complete': all(r['complete'] for r in results),
           'pin_held': all(r['pin_held'] for r in results)}
    for status in COMPARABLE:
        out['stages'][status] = summarise([v for r in results for v in r['samples'][status]])
    out['comparable'] = summarise([v for r in results for v in r['samples']['comparable_ms']])
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
        print(f'  generation committed on: {", ".join(result["generation_threads"])}'
              + ('' if result['pin_held'] else '  <-- THE PIN DID NOT HOLD'))
    sys.stdout.flush()


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


def thread_cpu(jstack, pid):
    """CPU milliseconds of every thread of @p pid, by name, from jstack."""
    out = subprocess.run([jstack, str(pid)], capture_output=True, text=True, check=False)
    if out.returncode != 0:
        die(f'jstack {pid} failed: {out.stderr.strip()}')
    cpu = {}
    for line in out.stdout.splitlines():
        found = re.match(r'"([^"]+)".*?\bcpu=([\d.]+)ms', line)
        if found:
            cpu[found.group(1)] = float(found.group(2))
    return cpu


def worker_ticks(pid):
    """Summed user+system clock ticks of the process's Worker-Main threads."""
    total = 0
    for task in pathlib.Path(f'/proc/{pid}/task').iterdir():
        try:
            stat = (task / 'stat').read_text(encoding='ascii', errors='replace')
        except OSError:
            continue
        name = stat[stat.index('(') + 1:stat.rindex(')')]
        if name.startswith(WORKER_PREFIX):
            fields = stat[stat.rindex(')') + 2:].split()
            total += int(fields[11]) + int(fields[12])
    return total


class Server:
    def __init__(self, directory, jdk, jar, seed, preset, threads, structures, heap):
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
        self.log_handle = open(self.log_path, 'w', encoding='utf-8')
        self.command = [jdk['java'], f'-Xmx{heap}', f'-Dmax.bg.threads={threads}', '-jar',
                        str(jar), '--nogui']
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

    def settle(self, quiet=3.0, timeout=1800.0):
        """Waits until the Worker-Main threads have spent no CPU for @p quiet seconds."""
        start = time.monotonic()
        last = worker_ticks(self.process.pid)
        still_since = time.monotonic()
        while time.monotonic() - start < timeout:
            time.sleep(0.5)
            if self.process.poll() is not None:
                die(f'the server exited (see {self.log_path})')
            now = worker_ticks(self.process.pid)
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
    server.send('jfr start')
    server.wait_for(r'JFR profiling started', 60, mark)
    load_before = load_average()
    log(f'{name}: generating the {2 * area[2] + 1}-chunk square around ({area[0]}, {area[1]})')
    began = time.monotonic()
    forceload(server, level, area)
    settled = server.settle()
    mark = len(server.log_text())
    server.send('jfr stop')
    found = server.wait_for(r'JFR profiling stopped and dumped to (\S+\.jfr)', 300, mark)
    elapsed = time.monotonic() - began
    load_after = load_average()
    after = thread_cpu(jdk['jstack'], server.process.pid)
    recording = server.directory / found.group(1)
    result = analyse(read_events(recording, jdk['jfr']), level, area, threads)
    result['dimension'] = name
    result['wall_s'] = elapsed
    result['settled_after_s'] = settled
    result['load_average_before'] = load_before
    result['load_average_after'] = load_after
    result['worker_cpu_s'] = {thread: (after.get(thread, 0.0) - before.get(thread, 0.0)) / 1000.0
                              for thread in sorted(after) if thread.startswith(WORKER_PREFIX)}
    result['jstack_workers'] = sorted(t for t in after if t.startswith(WORKER_PREFIX))
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
    java_version = subprocess.run([jdk['java'], '-version'], capture_output=True, text=True,
                                  check=False).stderr.splitlines()[0]
    work = pathlib.Path(args.work) if args.work else pathlib.Path(
        tempfile.mkdtemp(prefix='stratum-vanilla-cost-'))
    work.mkdir(parents=True, exist_ok=True)
    report = {'tool': 'vanilla-chunk-cost', 'minecraft_version': VERSION, 'jar': jar.name,
              'java': java_version, 'threads': args.threads, 'structures': args.structures,
              'heap': args.heap, 'area': list(args.area), 'warmup_area': list(args.warmup_area),
              'seeds': seeds, 'load_average_start': load_average(), 'runs': []}
    log(f'{java_version}; -Dmax.bg.threads={args.threads}; work in {work}')
    # One server per seed and preset: the overworld and the End share the
    # normal preset's.
    presets = []
    for name in names:
        if DIMENSIONS[name][0] not in presets:
            presets.append(DIMENSIONS[name][0])
    try:
        for seed in seeds:
            for preset in presets:
                directory = work / f'{preset.split(":")[1]}-seed{seed}'
                if directory.exists():
                    shutil.rmtree(directory)
                log(f'seed {seed}: starting the server ({preset})')
                server = Server(directory, jdk, jar, seed, preset, args.threads,
                                args.structures == 'on', args.heap)
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
                        cpu = sum(result['worker_cpu_s'].values())
                        busy = sum(t['busy_s'] for n, t in result['threads'].items()
                                   if n.startswith(WORKER_PREFIX))
                        print_result(f'{name} seed {seed} (load {result["load_average_after"]}; '
                                     f'worker CPU {cpu:.2f} s over the recording, the estimates '
                                     f'sum to {busy:.2f} s)', result)
                finally:
                    server.stop()
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    report['dimensions'] = {}
    for name in names:
        results = [r for r in report['runs'] if r['dimension'] == name]
        report['dimensions'][name] = pool(results)
        print_result(f'{name}, every seed, {len(results)} run(s)', report['dimensions'][name])
    report['load_average_end'] = load_average()
    print(f'load average at end: {report["load_average_end"]}')
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if not all(r['complete'] and r['pin_held'] for r in report['runs']):
        log('WARNING: a run was incomplete, or its pin did not hold; see above')
        return 3
    return 0


def analyze(args):
    jfr = None
    if pathlib.Path(args.recording).suffix == '.jfr':
        jfr = find_jdk(args.jdk)['jfr']
    result = analyse(read_events(args.recording, jfr), args.level, args.area, args.threads)
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
