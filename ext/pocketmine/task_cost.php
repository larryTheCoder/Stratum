<?php

declare(strict_types=1);

/*
 * Stratum — what a PocketMine-MP generation task costs per chunk THROUGH
 * PHP: the cost the C++-only bench (stratum_chunk_cost_bench) does not see.
 * Run by tools/pmmp-dev task-cost, under PocketMine-MP's own PHP with the
 * php.ini settings a tools/pmmp-dev server runs with (opcache as
 * PHP-Binaries enables it, assertions off), and driven by
 * tools/pmmp-task-cost, which runs the bench first and reads this one's
 * JSON.
 *
 *   php -d extension=stratum.so task_cost.php --phar=PocketMine-MP.phar
 *       --version-root=.fixtures/1.21.11 --dimension=overworld
 *       --seeds=0,42 --dumps=<the bench's --dump dir> --spiral-chunks=121
 *       [--warmup=1] [--repeat=3] [--json=FILE]
 *       [--count=SEGMENT --perf-control=CTL_FIFO,ACK_FIFO]
 *
 * THE SHAPE. Per seed, one world, as a server holds it: the version root
 * frozen into a blob (Stratum\freezePipeline, as WorldFactory does), the
 * options string the plugin's GeneratorOptions writes, the world pinned on
 * this (the main) thread as StratumWorlds pins it at WorldLoadEvent. Then
 * one generation thread (TaskCostThread, a pocketmine\thread\Thread) plays
 * an AsyncWorker: PocketMine-MP's own AsyncGeneratorRegisterTask builds the
 * plugin's StratumGenerator, and each chunk is PocketMine-MP's own
 * PopulationTask — onRun(), the worker's GarbageCollectorManager, and
 * onCompletion() — one chunk at a time, while this thread waits in join().
 * The chunks are the bench's, in its order, read from its dump: the 121
 * within 5 of chunk (-64, 64) and its 16 scattered ones.
 *
 * WHAT IT TIMES, per chunk, wall (hrtime) and CPU (getrusage, the process's,
 * which is the generation thread's while this thread waits; see Probe):
 *
 *   engine      (i)+(ii)  StratumGenerator::encode(): Stratum\Dimension::
 *                         encodeChunk, the zend method — the C++
 *                         stratum::pmmp::encodeChunk (what the bench times),
 *                         the marshaling of its result into PHP arrays and
 *                         the put of its fluid updates into the outbox.
 *                         tools/pmmp-task-cost splits it in two from a perf
 *                         profile of this segment alone.
 *   plugin      (iii)     StratumGenerator::assemble() and the ChunkManager's
 *                         setChunk(): palette translation, a
 *                         PalettedBlockArray::fromData per layer, the SubChunk
 *                         and Chunk construction.
 *   pmmp        (iv)      the rest of PopulationTask::onRun(): the context
 *                         lookup, the SimpleChunkManager and its placeholder
 *                         chunk, populateChunk() (nothing, for Stratum),
 *                         setPopulated(), and the serialisation of the result
 *                         into the task (FastChunkSerializer, igbinary, the
 *                         task's thread-safe properties).
 *   serialize             FastChunkSerializer::serializeTerrain() of the
 *                         chunk on its own: the main part of pmmp, shown apart.
 *   task                  PopulationTask::onRun() over the plugin's own
 *                         StratumGenerator, no probe inside: the end-to-end
 *                         sweep. split_task is the same over TimedGenerator,
 *                         whose three probes it includes.
 *   gc                    GarbageCollectorManager::maybeCollectCycles(), which
 *                         AsyncTask::run() calls after every task on a worker.
 *   worker                task + gc: the generation thread's whole task.
 *   completion            PopulationTask::onCompletion(): the main thread's
 *                         side, deserialising the chunk. Not the generation
 *                         thread's time: reported apart.
 *
 * Each chunk's time for a segment is its median over the timed passes; the
 * figures are then median, p10, p90 and mean ms/chunk over chunks, and
 * chunks/s as 1000 / mean, as the bench reports them.
 *
 * LEFT OUT. AsyncTask::run()'s own framework — its timings handler (off,
 * so nothing) and the SleeperNotifier it wakes, a write to the main
 * thread's notifier — and AsyncPool's queueing of a task to a worker and
 * collection of its result on the main thread: PocketMine-MP's cost of any
 * async task, not the generator's. The copy of the result between threads
 * that pmmpthread makes when the main thread reads the task's properties
 * is read on this same thread here, so it is a read, not a copy. The fluid
 * updates' hand-over to the main thread (Stratum\takeFluidUpdates, the
 * plugin's FluidUpdateScheduler) is the main thread's, after the task: it
 * is taken outside every span. A population task's adjacent chunks: a
 * server's task generates every missing chunk of the 3x3 and hands each
 * back; this runs one chunk a task, which is the same per-chunk work
 * without the 3x3's bookkeeping (igbinary over the neighbours' serialised
 * strings). Nor the neighbours a task is handed already generated: a chunk
 * is populated once its 3x3 exists, and its task carries those of the eight
 * that are generated to the worker — serialised on the main thread,
 * deserialised on the worker, not sent back unless changed — population's
 * cost, whatever the generator (Stratum's populateChunk() does nothing), up
 * to eight times the serialize and completion figures here per populated
 * chunk. Lighting, which PocketMine-MP does after.
 *
 * THE CHECK, before anything is timed: on the first pass every chunk of
 * both sweeps must be what the C++ produced (TaskCostThread::check) — what
 * encode() returned identical to the bench's dump, the chunk onCompletion()
 * handed back identical sub-chunk for sub-chunk with each block palette the
 * plugin's translation of the C++'s, and the fluid updates identical. Any
 * difference fails the run.
 *
 * In a counting run (--count, under `perf stat` or `perf record` with
 * `--control` and `-D -1`), perf's events are on for one segment alone in
 * the timed passes, and nothing is timed: the per-chunk instruction counts
 * tools/pmmp-task-cost reports.
 *
 * Nothing it writes is Mojang-derived; the blob it freezes is removed when
 * it ends, and the dumps it reads stay where the bench wrote them.
 */

namespace Stratum\PocketMineEvidence;

use Stratum\PMMP\GeneratorOptions;
use function array_keys;
use function array_map;
use function array_sum;
use function count;
use function explode;
use function file_put_contents;
use function floor;
use function implode;
use function ini_get;
use function intdiv;
use function is_file;
use function json_decode;
use function json_encode;
use function min;
use function phpversion;
use function printf;
use function sort;
use function str_contains;
use const JSON_PRETTY_PRINT;
use const JSON_THROW_ON_ERROR;
use const JSON_UNESCAPED_SLASHES;
use const PHP_DEBUG;
use const PHP_VERSION;
use const PHP_ZTS;

require __DIR__ . "/bootstrap.php";

/** The dimensions the bench knows, as [noise settings, biome source]: vanilla's world presets. */
const DIMENSIONS = [
	"overworld" => ["minecraft:overworld", "minecraft:overworld"],
	"amplified" => ["minecraft:amplified", "minecraft:overworld"],
	"large_biomes" => ["minecraft:large_biomes", "minecraft:overworld"],
	"end" => ["minecraft:end", "minecraft:the_end"]
];

/** Every segment reported, in order, with what it is. */
const SEGMENTS = [
	"engine" => "(i)+(ii) encode(): the zend encodeChunk",
	"plugin" => "(iii) assemble() + setChunk(): the plugin's PHP",
	"pmmp" => "(iv) the rest of PopulationTask::onRun",
	"serialize" => "  of which serializeTerrain alone",
	"split_task" => "onRun, split sweep (3 probes inside)",
	"task" => "onRun, end to end",
	"gc" => "the worker's collector",
	"worker" => "the generation thread's task (onRun + collector)",
	"completion" => "main thread: onCompletion (deserialise)"
];

/**
 * @param list<float> $sorted
 */
function percentile(array $sorted, float $p) : float{
	$rank = $p * (count($sorted) - 1);
	$low = (int) floor($rank);
	$high = min($low + 1, count($sorted) - 1);
	return $sorted[$low] + ($sorted[$high] - $sorted[$low]) * ($rank - $low);
}

/**
 * @param list<int|float> $values
 */
function median(array $values) : float{
	sort($values);
	$n = count($values);
	return $n % 2 === 1 ? (float) $values[intdiv($n, 2)] : ($values[intdiv($n, 2) - 1] + $values[intdiv($n, 2)]) / 2;
}

/**
 * ms/chunk over chunks, each value one chunk's median over passes, in ns.
 *
 * @param list<float> $perChunkNs
 *
 * @return array<string, int|float|bool>
 */
function summarise(array $perChunkNs) : array{
	if(count($perChunkNs) === 0){
		return ["available" => false, "chunks" => 0];
	}
	sort($perChunkNs);
	$ms = array_map(static fn(float $ns) : float => $ns / 1e6, $perChunkNs);
	$mean = array_sum($ms) / count($ms);
	return [
		"available" => true,
		"chunks" => count($ms),
		"median_ms" => percentile($ms, 0.5),
		"p10_ms" => percentile($ms, 0.1),
		"p90_ms" => percentile($ms, 0.9),
		"mean_ms" => $mean,
		"chunks_per_s" => $mean > 0 ? 1000 / $mean : 0
	];
}

/**
 * Every segment's figures over the chunks $include picks, across seeds.
 *
 * @param list<array<string, mixed>> $runs
 * @param \Closure(int) : bool $include by chunk index
 *
 * @return array<string, array{wall: array<string, mixed>, cpu: array<string, mixed>}>
 */
function table(array $runs, \Closure $include) : array{
	$out = [];
	foreach(array_keys(SEGMENTS) as $segment){
		$wall = [];
		$cpu = [];
		foreach($runs as $run){
			foreach($run["samples"][$segment] ?? [] as $i => $spans){
				if($include($i)){
					$wall[] = median(array_map(static fn(array $s) : int => $s[0], $spans));
					$cpu[] = median(array_map(static fn(array $s) : int => $s[1], $spans));
				}
			}
		}
		$out[$segment] = ["wall" => summarise($wall), "cpu" => summarise($cpu)];
	}
	return $out;
}

/**
 * @param array<string, mixed> $figure
 */
function line(string $label, array $figure) : void{
	if(!$figure["available"]){
		printf("  %-50s -\n", $label);
		return;
	}
	printf("  %-50s median %8.3f  p10 %8.3f  p90 %8.3f  mean %8.3f ms/chunk  %8.2f chunks/s\n",
		$label, $figure["median_ms"], $figure["p10_ms"], $figure["p90_ms"], $figure["mean_ms"], $figure["chunks_per_s"]);
}

$args = arguments($argv, ["phar", "version-root", "dimension", "seeds", "dumps", "spiral-chunks"],
	["warmup", "repeat", "json", "count", "perf-control"]);
$loader = bootstrap($args["phar"]);
$dimension = $args["dimension"];
if(!isset(DIMENSIONS[$dimension])){
	fail("--dimension is one of " . implode(", ", array_keys(DIMENSIONS)) . ", not " . $dimension);
}
[$noiseSettings, $biomeSource] = DIMENSIONS[$dimension];
$seeds = array_map(intval(...), explode(",", $args["seeds"]));
$warmup = (int) ($args["warmup"] ?? 1);
$repeat = (int) ($args["repeat"] ?? 3);
$spiralChunks = (int) $args["spiral-chunks"];
$counted = $args["count"] ?? "";
if($warmup < 0 || $repeat < 1){
	fail("--warmup must be at least 0 and --repeat at least 1");
}
[$controlFifo, $ackFifo] = ["", ""];
if($counted !== ""){
	if(!isset(TaskCostThread::COUNTED[$counted])){
		fail("--count is one of " . implode(", ", array_keys(TaskCostThread::COUNTED)) . ", not " . $counted);
	}
	$fifos = explode(",", $args["perf-control"] ?? "");
	if(count($fifos) !== 2){
		fail("--count needs --perf-control=CTL_FIFO,ACK_FIFO, perf's --control fifos");
	}
	[$controlFifo, $ackFifo] = $fifos;
}

// The settings it ran under: a measurement taken with assertions on or
// opcache off would not be PocketMine-MP's.
$ini = [];
foreach(["opcache.enable", "opcache.enable_cli", "opcache.optimization_level", "opcache.jit", "opcache.jit_buffer_size", "zend.assertions", "recursionguard.enabled", "memory_limit"] as $setting){
	$value = ini_get($setting);
	$ini[$setting] = $value === false ? null : $value;
}
if($counted === "" && ($ini["zend.assertions"] !== "-1" || $ini["opcache.enable_cli"] !== "1")){
	fail("timing needs PocketMine-MP's own settings — zend.assertions=-1 and opcache on — not zend.assertions="
		. $ini["zend.assertions"] . " and opcache.enable_cli=" . $ini["opcache.enable_cli"] . " (run it through tools/pmmp-dev task-cost)");
}

$blob = Sample::freeze($args["version-root"]);
$options = GeneratorOptions::encode($blob, $noiseSettings, $biomeSource);
$runs = [];
$failures = [];
foreach($seeds as $seed){
	$dumpPath = $args["dumps"] . "/" . $dimension . "_seed" . $seed . ".dump";
	if(!is_file($dumpPath)){
		fail("no bench dump at " . $dumpPath . " (stratum_chunk_cost_bench --dump)");
	}
	\Stratum\pinWorld($blob, $noiseSettings, $biomeSource, $seed);
	$thread = new TaskCostThread($blob, $noiseSettings, $biomeSource, $seed, $options, $dumpPath, $warmup, $repeat, $counted, $controlFifo, $ackFifo);
	$thread->setClassLoaders([$loader]);
	$thread->start();
	$thread->join();
	$crash = $thread->getCrashInfo();
	\Stratum\unpinWorld($blob, $noiseSettings, $biomeSource, $seed);
	if($crash !== null){
		fail($dimension . " seed " . $seed . ": the generation thread crashed: " . $crash->makePrettyMessage());
	}
	if($thread->result === ""){
		fail($dimension . " seed " . $seed . ": the generation thread ended without a result");
	}
	/** @var array<string, mixed> $run */
	$run = json_decode($thread->result, true, 512, JSON_THROW_ON_ERROR);
	$thread->quit();
	$runs[] = $run;

	$checks = $run["checks"];
	printf("%s seed %d: %d chunk task(s) checked against the C++ (%d sub-chunks, %d fluid updates): %s\n",
		$dimension, $seed, $checks["chunks"], $checks["sub_chunks"], $checks["fluid_updates"],
		$checks["differences"] === 0 ? "identical" : $checks["differences"] . " DIFFERENCE(S)");
	foreach($checks["listed"] as $difference){
		printf("  %s\n", $difference);
	}
	if($checks["differences"] !== 0){
		$failures[] = $dimension . " seed " . $seed . ": " . $checks["differences"] . " difference(s) from the C++'s output";
	}
	if($checks["chunks"] !== 2 * count($run["chunks"]) && $counted === ""){
		$failures[] = $dimension . " seed " . $seed . ": " . $checks["chunks"] . " chunk task(s) checked, not " . 2 * count($run["chunks"]);
	}
	foreach($run["warnings"] as $warning){
		printf("  logged: %s\n", $warning);
		// The plugin's documented fallback (powder snow to snow) is a
		// warning by design; anything else is not.
		if(!str_contains($warning, "Stratum: PocketMine-MP cannot place minecraft:powder_snow")){
			$failures[] = $dimension . " seed " . $seed . " logged: " . $warning;
		}
	}
	printf("  worker setup (AsyncGeneratorRegisterTask: the generator, its pinned Dimension, its translator): %.1f ms; probe cost %.0f ns wall, %.0f ns CPU; collector runs %d\n",
		$run["setup"][0] / 1e6, $run["probe_cost"][0], $run["probe_cost"][1], $run["gc_runs"]);
	if($counted !== ""){
		printf("  counted: %s, in %d task window(s)\n", $counted, $run["windows"]);
	}
}

$report = [
	"tool" => "ext/pocketmine/task_cost.php",
	"dimension" => $dimension,
	"noise_settings" => $noiseSettings,
	"biome_source" => $biomeSource,
	"seeds" => $seeds,
	"warmup_passes" => $warmup,
	"timed_passes" => $repeat,
	"spiral_chunks" => $spiralChunks,
	"php" => PHP_VERSION . (PHP_ZTS ? " ZTS" : " NTS") . (PHP_DEBUG ? " debug" : ""),
	"stratum" => phpversion("stratum"),
	"ini" => $ini,
	"counted" => $counted,
	"runs" => array_map(static fn(array $run) : array => [
		"seed" => $run["seed"],
		"chunks" => count($run["chunks"]),
		"setup_ms" => $run["setup"][0] / 1e6,
		"probe_cost_ns" => $run["probe_cost"],
		"gc_runs" => $run["gc_runs"],
		"windows" => $run["windows"],
		"enables" => $run["enables"],
		"checks" => $run["checks"],
		"warnings" => $run["warnings"]
	], $runs),
	"failures" => $failures
];
if($counted === ""){
	$report["all"] = table($runs, static fn(int $i) : bool => true);
	$report["spiral"] = table($runs, static fn(int $i) : bool => $i < $spiralChunks);
	$report["scattered"] = table($runs, static fn(int $i) : bool => $i >= $spiralChunks);
	$report["per_seed"] = array_map(static fn(array $run) : array => ["seed" => $run["seed"], "all" => table([$run], static fn(int $i) : bool => true)], $runs);
	printf("%s, every seed, %d chunk(s) each, %d timed pass(es) — wall clock:\n", $dimension, count($runs[0]["chunks"]), $repeat);
	foreach(SEGMENTS as $segment => $label){
		line($label, $report["all"][$segment]["wall"]);
	}
	printf("  CPU (the process's; the generation thread's alone while the main thread waits):\n");
	foreach(SEGMENTS as $segment => $label){
		line($label, $report["all"][$segment]["cpu"]);
	}
}
if(isset($args["json"])){
	file_put_contents($args["json"], json_encode($report, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . "\n");
}
if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
