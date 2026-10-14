<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use pmmp\thread\ThreadSafeArray;
use pocketmine\thread\Thread;
use function count;
use function explode;
use function hrtime;
use function serialize;

/**
 * One PocketMine-MP thread — pocketmine\thread\Thread, the class 5.44.3
 * builds its own on, over pmmpthread — that opens a world's Dimension from
 * its frozen blob, as StratumGenerator does on a generation worker, and
 * encodes chunks, leaving each chunk's serialized encodeChunk() in
 * $results for the thread that started it to compare.
 *
 * Each thread walks the same chunks from a different starting point, so the
 * threads are encoding different chunks of the same world at once, and the
 * same chunk at different times.
 */
final class EncodeThread extends Thread{

	/** @var ThreadSafeArray<string, string> "x,z" => serialize(encodeChunk(x, z)) */
	public ThreadSafeArray $results;

	public int $startedAt = 0;

	public int $finishedAt = 0;

	/**
	 * @param string $chunks "x,z;x,z;..."
	 */
	public function __construct(
		private string $blob,
		private string $settings,
		private string $biomes,
		private int $seed,
		private string $chunks,
		private int $offset
	){
		$this->results = new ThreadSafeArray();
	}

	protected function onRun() : void{
		$dimension = \Stratum\Dimension::open($this->blob, $this->settings, $this->biomes, $this->seed);
		$chunks = explode(";", $this->chunks);
		$count = count($chunks);
		$this->startedAt = hrtime(true);
		for($i = 0; $i < $count; $i++){
			$key = $chunks[($i + $this->offset) % $count];
			[$x, $z] = explode(",", $key);
			$this->results[$key] = serialize($dimension->encodeChunk((int) $x, (int) $z));
		}
		$this->finishedAt = hrtime(true);
	}
}
