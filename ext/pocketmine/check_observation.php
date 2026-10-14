<?php

declare(strict_types=1);

/*
 * Stratum — what tools/pmmp-dev observe saw a real PocketMine-MP 5.44.3 do,
 * read from the server's own log and the evidence plugin's report
 * (src/EvidencePlugin.php), against what the plugin under test needs:
 *
 *   php check_observation.php --server-log=<server.log>
 *       --report=<stratum-evidence-report.json>
 *       --fluid-updates=<inspect_world.php's --fluid-updates file> --world=<name>
 *
 *  1. The world's WorldLoadEvent fired with none of its chunks loaded, and
 *     pinned by StratumWorlds' own record (it pins at LOWEST), before its
 *     first chunk was requested — the first "Registering generator on
 *     worker" line, which AsyncGeneratorExecutor::populate() logs as it
 *     submits the first population task — and before its first
 *     ChunkLoadEvent. server.log is written by the main thread in the order
 *     these happen there.
 *  2. PocketMine-MP shut every async worker down: MemoryManager logged
 *     "Shut down N idle async pool workers" for as many workers as were
 *     running before, and none was running after, the world still pinned
 *     by StratumWorlds' record.
 *  3. A worker started afterwards registered the generator again.
 *  4. Every chunk ordered populated after the shutdown was populated, and
 *     every fluid position the engine marks in those chunks
 *     (inspect_world.php encodes each and takes them, in a process of its
 *     own) had a block update scheduled: the updates the old workers left
 *     when they generated those chunks, before the shutdown, held by the
 *     pin and delivered once the chunks had settled — populated, with
 *     every chunk within two of them populated too, which the evidence
 *     plugin orders as well. At least one such
 *     position, or nothing was shown. This, not StratumWorlds' record, is
 *     what shows the extension's pin held — the extension cannot be asked
 *     whether a world is pinned without changing its pins — and a plugin
 *     that records a pin and never makes one fails here.
 *  5. At every `stratumevidence status`, the plugin's FluidUpdateScheduler
 *     held no more chunks than the world had loaded: it lets a chunk go as
 *     it unloads, so what waits on terrain no one returns to does not pile
 *     up. Printed at each, with how many it had due.
 */

namespace Stratum\PocketMineEvidence;

require __DIR__ . "/bootstrap.php";

$args = arguments($argv, ["server-log", "report", "fluid-updates", "world"]);
$lines = @file($args["server-log"], FILE_IGNORE_NEW_LINES);
if($lines === false){
	fail("no server.log at " . $args["server-log"]);
}
$report = json_decode((string) @file_get_contents($args["report"]), associative: true);
if(!is_array($report)){
	fail("no evidence plugin report at " . $args["report"] . ": the plugin did not run, or was not disabled cleanly");
}
$marked = json_decode((string) @file_get_contents($args["fluid-updates"]), associative: true);
if(!is_array($marked)){
	fail("no fluid updates from inspect_world.php at " . $args["fluid-updates"]);
}
$world = $args["world"];
$failures = [];

/** The index of the first server.log line holding $needle at or after $from, or null. */
$first = static function(string $needle, int $from = 0) use ($lines) : ?int{
	for($i = $from, $n = count($lines); $i < $n; $i++){
		if(str_contains($lines[$i], $needle)){
			return $i;
		}
	}
	return null;
};
$lineNo = static fn(?int $index) : string => $index === null ? "never" : "server.log line " . ($index + 1);
$yesNo = static fn(mixed $value) : string => $value === true ? "yes" : ($value === false ? "no" : "cannot tell");

// --- 1. WorldLoadEvent before the first chunk request ----------------------

$load = $first("[StratumEvidence] WorldLoadEvent for world \"" . $world . "\"");
$request = $first("Registering generator on worker");
$firstChunk = $first("[StratumEvidence] first ChunkLoadEvent for world \"" . $world . "\"");
$worldLoad = $report["worldLoad"] ?? null;
echo "WorldLoadEvent for ", $world, ": ", $lineNo($load), "; chunks loaded then: ", $worldLoad["loadedChunks"] ?? "?",
	"; pinned then, by StratumWorlds' record: ", $yesNo($worldLoad["pinned"] ?? null), "\n";
echo "its first chunk request (the generator's first registration on a worker): ", $lineNo($request), "\n";
echo "its first ChunkLoadEvent: ", $lineNo($firstChunk), "\n";
if($load === null || !is_array($worldLoad)){
	$failures[] = "the evidence plugin never saw the world's WorldLoadEvent";
}else{
	if($request === null || $request < $load){
		$failures[] = "the world's first chunk was requested before its WorldLoadEvent fired";
	}
	if($firstChunk === null || $firstChunk < $load){
		$failures[] = "a chunk of the world was loaded before its WorldLoadEvent fired";
	}
	if(($worldLoad["loadedChunks"] ?? -1) !== 0){
		$failures[] = "the world had chunks loaded when its WorldLoadEvent fired";
	}
	if(($worldLoad["pinned"] ?? null) !== true){
		$failures[] = "the world was not pinned by the time its WorldLoadEvent's MONITOR handlers ran";
	}
}

// --- 2. every async worker shut down, the world still pinned -----------------

$statuses = $report["status"] ?? [];
$shutdown = null;
$shutDown = 0;
foreach($lines as $i => $line){
	if(preg_match('/Shut down (\d+) idle async pool workers/', $line, $m) === 1){
		$shutdown = $i;
		$shutDown = (int) $m[1];
	}
}
$before = count($statuses) >= 2 ? $statuses[count($statuses) - 2] : null;
$after = count($statuses) >= 1 ? $statuses[count($statuses) - 1] : null;
echo "async workers running before the console's gc: [", implode(",", $before["runningWorkers"] ?? []), "] of ", $before["poolSize"] ?? "?",
	", idle since ", $before["lastUsed"] ?? "?", " (", isset($before["lastUsed"], $before["time"]) ? $before["time"] - $before["lastUsed"] : "?", "s)\n";
echo "PocketMine-MP's MemoryManager: ", $shutdown === null ? "never shut a worker down" : "shut down " . $shutDown . " idle async pool worker(s), " . $lineNo($shutdown), "\n";
echo "async workers running after: [", implode(",", $after["runningWorkers"] ?? ["?"]), "]; pinned, by StratumWorlds' record: ", $yesNo($after["pinned"] ?? null), "\n";
if($before === null || $after === null || $shutdown === null){
	$failures[] = "no worker shutdown was observed";
}else{
	if(count($before["runningWorkers"]) === 0 || $shutDown !== count($before["runningWorkers"])){
		$failures[] = "PocketMine-MP shut down " . $shutDown . " of the " . count($before["runningWorkers"]) . " running async workers";
	}
	if($after["runningWorkers"] !== []){
		$failures[] = "async workers were still running after the shutdown: " . implode(",", $after["runningWorkers"]);
	}
	if(($after["pinned"] ?? null) !== true){
		$failures[] = "the world was not pinned after its workers were shut down";
	}
}

// --- 3. the generator registered again on a new worker -----------------------

$again = [];
if($shutdown !== null){
	for($i = $shutdown + 1, $n = count($lines); $i < $n; $i++){
		if(preg_match('/Registering generator on worker (\d+)/', $lines[$i], $m) === 1){
			$again[$m[1]] = true;
		}
	}
}
echo "generator registered again after the shutdown on worker(s): ", count($again) > 0 ? implode(",", array_keys($again)) : "none", "\n";
if($shutdown !== null && count($again) === 0){
	$failures[] = "no worker registered the generator again after the shutdown";
}

// --- 4. the held fluid updates delivered ---------------------------------------

$populated = $report["populated"] ?? [];
$ordered = $report["ordered"] ?? 0;
echo "chunks next to the spawn area, generated before the shutdown and populated after it: ", $ordered, " ordered, ",
	$report["completed"] ?? 0, " populated; and within two of them, so that they settle: ", $report["settleOrdered"] ?? 0,
	" ordered, ", $report["settleCompleted"] ?? 0, " populated\n";
if($ordered < 1){
	$failures[] = "no chunk generated before the shutdown was left to populate after it";
}elseif(($report["completed"] ?? 0) !== $ordered){
	$failures[] = "only " . ($report["completed"] ?? 0) . " of the " . $ordered . " chunks ordered populated were";
}
if(($report["settleCompleted"] ?? 0) !== ($report["settleOrdered"] ?? 0)){
	$failures[] = "only " . ($report["settleCompleted"] ?? 0) . " of the " . ($report["settleOrdered"] ?? 0) .
		" chunks ordered populated round them were";
}
$markedTotal = 0;
$deliveredTotal = 0;
$atEvent = 0;
$short = [];
foreach($populated as $chunk => $observed){
	$seen = [];
	foreach($observed["seen"] ?? [] as [$x, $y, $z]){
		$seen[$x . "," . $y . "," . $z] = true;
	}
	$expected = $marked[$chunk] ?? null;
	if(!is_array($expected)){
		$failures[] = "inspect_world.php did not encode chunk " . $chunk;
		continue;
	}
	$delivered = 0;
	foreach($expected as [$x, $y, $z]){
		if(isset($seen[$x . "," . $y . "," . $z])){
			$delivered++;
		}
	}
	$markedTotal += count($expected);
	$deliveredTotal += $delivered;
	$atEvent += $observed["scheduledByHandlers"] ?? 0;
	if($delivered !== count($expected)){
		$short[] = $chunk . " (" . $delivered . " of " . count($expected) . ")";
	}
}
echo "fluid positions the engine marks in them: ", $markedTotal, "; with a block update scheduled: ", $deliveredTotal,
	" (", $atEvent, " block update(s) scheduled by ChunkPopulateEvent handlers as they ran)\n";
if(count($short) > 0){
	$failures[] = "marked fluid positions left without a block update in " . count($short) . " chunk(s): " . implode(" ", array_slice($short, 0, 10));
}
if($markedTotal === 0 && $ordered > 0){
	$failures[] = "the engine marks no fluid position in the chunks populated after the shutdown, so nothing shows the pin kept any";
}

// --- 5. the fluid scheduler holds only loaded chunks ---------------------------

$scheduler = [];
foreach($statuses as $status){
	$held = $status["chunksHeld"] ?? null;
	$scheduler[] = ($held === null ? "?" : $held . " held, " . ($status["chunksDue"] ?? "?") . " due") . " of " . ($status["loadedChunks"] ?? "?") . " loaded";
	if($held === null){
		$failures[] = "the fluid scheduler's chunks could not be read at a status";
	}elseif($held > ($status["loadedChunks"] ?? -1)){
		$failures[] = "the fluid scheduler held " . $held . " chunks with " . ($status["loadedChunks"] ?? "?") . " loaded";
	}
}
echo "the fluid scheduler at each status: ", count($scheduler) > 0 ? implode("; ", $scheduler) : "never asked", "\n";

if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
