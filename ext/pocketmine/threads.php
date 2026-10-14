<?php

declare(strict_types=1);

/*
 * Stratum — several real PocketMine-MP threads open the same world's
 * Dimension from one frozen blob and encode chunks at once; every byte must
 * match a single-threaded encode (M5 Stage 0; SPEC sections 4.2 and 4.3).
 *
 *   php -d extension=stratum.so threads.php --phar=PocketMine-MP.phar
 *       --version-root=.fixtures/1.21.11 [--threads=4] [--side=4]
 *
 * This is the shape of a PocketMine-MP server's generation: one generator
 * per worker thread, each in its own PHP request, each opening the world's
 * Dimension, the extension sharing one compiled dimension between them. The
 * threads are pocketmine\thread\Thread — PocketMine-MP's own class, with
 * its own class loader registration — over pmmpthread.
 *
 * The threads start first, with nothing open, so they race to open (and
 * compile) the dimension as workers do when a world loads. Only after they
 * are joined — and their Dimensions released, so the extension's shared
 * dimension goes with them — does this thread open its own and encode the
 * reference. Each thread's every chunk must be byte-identical to it:
 * serialize() of encodeChunk()'s whole answer, blocks, biomes and keys.
 * The threads' encoding windows are reported, to show they overlapped.
 */

namespace Stratum\PocketMineEvidence;

require __DIR__ . "/bootstrap.php";

$args = arguments($argv, ["phar", "version-root"], ["threads", "side"]);
$loader = bootstrap($args["phar"]);
$threadCount = (int) ($args["threads"] ?? 4);
$side = (int) ($args["side"] ?? 4);
if($threadCount < 2 || $side < 1){
	fail("--threads must be at least 2 and --side at least 1");
}

$blob = Sample::freeze($args["version-root"]);
$failures = [];
foreach(Sample::DIMENSIONS as $label => [$settings, $biomes]){
	// Neighbouring chunks and far ones, negative coordinates included.
	$chunks = [];
	foreach(Sample::grid($side, 7) as [$x, $z]){
		$chunks[] = ($x + 1) . "," . ($z - 2);
	}
	$chunkList = implode(";", $chunks);

	/** @var list<EncodeThread> $threads */
	$threads = [];
	for($i = 0; $i < $threadCount; $i++){
		$thread = new EncodeThread($blob, $settings, $biomes, Sample::SEED, $chunkList, intdiv($i * count($chunks), $threadCount));
		$thread->setClassLoaders([$loader]);
		$threads[] = $thread;
	}
	foreach($threads as $thread){
		$thread->start();
	}
	foreach($threads as $thread){
		$thread->join();
	}

	$reference = [];
	$dimension = \Stratum\Dimension::open($blob, $settings, $biomes, Sample::SEED);
	foreach($chunks as $key){
		[$x, $z] = explode(",", $key);
		$reference[$key] = serialize($dimension->encodeChunk((int) $x, (int) $z));
	}
	unset($dimension);

	$identical = 0;
	$windows = [];
	foreach($threads as $i => $thread){
		$crash = $thread->getCrashInfo();
		if($crash !== null){
			$failures[] = $label . " thread " . $i . " crashed: " . $crash->makePrettyMessage();
			continue;
		}
		foreach($chunks as $key){
			$got = $thread->results[$key] ?? null;
			if($got === null){
				$failures[] = $label . " thread " . $i . " has no result for chunk " . $key;
			}elseif($got !== $reference[$key]){
				$failures[] = $label . " thread " . $i . " encoded chunk " . $key . " differently from the single-threaded encode (" . strlen($got) . " bytes against " . strlen($reference[$key]) . ")";
			}else{
				$identical++;
			}
		}
		$windows[] = [$thread->startedAt, $thread->finishedAt];
		$thread->quit();
	}

	// How many threads were encoding at the busiest instant.
	$busiest = 0;
	foreach($windows as [$start]){
		$busiest = max($busiest, count(array_filter($windows, static fn(array $w) : bool => $w[0] <= $start && $start < $w[1])));
	}
	$bytes = array_sum(array_map("strlen", $reference));
	echo $label, ": ", $threadCount, " pocketmine\\thread\\Thread x ", count($chunks), " chunks, ", $identical, " of ", $threadCount * count($chunks),
		" encodes byte-identical to the single-threaded encode (", $bytes, " serialized bytes per pass); at most ", $busiest, " threads encoding at once\n";
	if($busiest < 2){
		$failures[] = $label . ": the threads never encoded at the same time, so this checked nothing about concurrency";
	}
}
unlink($blob);

if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
