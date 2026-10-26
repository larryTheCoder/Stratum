<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use function fclose;
use function fopen;
use function fread;
use function ord;
use function strlen;
use function substr;
use function unpack;

/**
 * One file of stratum_chunk_cost_bench's --dump (tools/analysis/
 * chunk-cost-bench.cpp, DumpFile): what the C++ alone produced for every
 * chunk of one dimension at one seed, in the bench's chunk order. Read here
 * so that the generation task timed through PHP runs the bench's very
 * chunks, in its order, and so that what the task hands PocketMine-MP can
 * be held to what the C++ produced, chunk by chunk (TaskCostThread).
 *
 * Only what that check needs is kept: each chunk's position, its 24 encoded
 * sub-chunks exactly as stratum::pmmp::encodeChunk returned them, and its
 * fluid updates; the per-block Java states and the biome indices are
 * skipped. The bench writes host-endian, so a dump is read on the machine
 * that wrote it, as the bench's own --compare requires.
 *
 * A dump is vanilla-derived (SPEC section 12): it is read where the
 * instrument wrote it, outside anything tracked, and never copied.
 */
final class ChunkDump{

	private const MAGIC = "STRATUMCHUNKDUMP";

	private const FORMAT = 1;

	public readonly string $dimension;

	public readonly int $seed;

	public readonly int $engineVersion;

	/** @var list<array{int, int}> every chunk's position, in the bench's order */
	public readonly array $chunks;

	/**
	 * @var list<array{subChunks: array<int, array{blocks: array{int, string, list<int>}|null, biomes: array{int, string, list<int>}}>, fluid: list<array{int, int, int}>}>
	 * per chunk, in the same order: the sub-chunks keyed by index, each layer
	 * as [bitsPerBlock, the words' host-endian bytes, palette] — the triple
	 * Stratum\Dimension::encodeChunk() returns — and the fluid updates as
	 * world [x, y, z]
	 */
	public readonly array $records;

	public function __construct(string $path){
		$file = @fopen($path, "rb");
		if($file === false){
			fail("cannot read the bench's dump " . $path);
		}
		try{
			$header = self::read($file, strlen(self::MAGIC) + 12, $path);
			if(substr($header, 0, strlen(self::MAGIC)) !== self::MAGIC){
				fail($path . " is not a stratum_chunk_cost_bench dump");
			}
			/** @var array{format: int, engine: int, name: int} $fields */
			$fields = unpack("Vformat/Vengine/Vname", $header, strlen(self::MAGIC));
			if($fields["format"] !== self::FORMAT){
				fail($path . " is dump format " . $fields["format"] . ", not " . self::FORMAT . ", which this reads");
			}
			$this->engineVersion = $fields["engine"];
			$this->dimension = self::read($file, $fields["name"], $path);
			/** @var array{seed: int, chunks: int} $tail */
			$tail = unpack("qseed/Vchunks", self::read($file, 12, $path));
			$this->seed = $tail["seed"];

			$chunks = [];
			$records = [];
			for($i = 0; $i < $tail["chunks"]; $i++){
				/** @var array{length: int} $length */
				$length = unpack("Plength", self::read($file, 8, $path));
				[$position, $record] = self::record(self::read($file, $length["length"], $path), $path);
				$chunks[] = $position;
				$records[] = $record;
			}
			$this->chunks = $chunks;
			$this->records = $records;
		}finally{
			fclose($file);
		}
	}

	/**
	 * @param resource $file
	 */
	private static function read($file, int $length, string $path) : string{
		if($length === 0){
			return "";
		}
		$bytes = fread($file, $length);
		if($bytes === false || strlen($bytes) !== $length){
			fail($path . " ends early");
		}
		return $bytes;
	}

	/**
	 * @return array{array{int, int}, array{subChunks: array<int, array{blocks: array{int, string, list<int>}|null, biomes: array{int, string, list<int>}}>, fluid: list<array{int, int, int}>}}
	 */
	private static function record(string $record, string $path) : array{
		/** @var array{x: int, z: int, blocks: int} $head */
		$head = unpack("lx/lz/Vblocks", $record);
		[$chunkX, $chunkZ] = [$head["x"], $head["z"]];
		$offset = 12 + 4 * $head["blocks"];
		/** @var array{biomes: int} $biomes */
		$biomes = unpack("Vbiomes", $record, $offset);
		$offset += 4 + 2 * $biomes["biomes"];
		/** @var array{count: int} $count */
		$count = unpack("Vcount", $record, $offset);
		$offset += 4;
		$subChunks = [];
		for($i = 0; $i < $count["count"]; $i++){
			/** @var array{index: int} $index */
			$index = unpack("lindex", $record, $offset);
			$hasBlocks = ord($record[$offset + 4]);
			$offset += 5;
			$blocks = null;
			if($hasBlocks === 1){
				$blocks = self::layer($record, $offset);
			}
			$subChunks[$index["index"]] = ["blocks" => $blocks, "biomes" => self::layer($record, $offset)];
		}
		/** @var array{count: int} $fluidCount */
		$fluidCount = unpack("Vcount", $record, $offset);
		$offset += 4;
		$fluid = [];
		for($i = 0; $i < $fluidCount["count"]; $i++){
			/** @var array{x: int, y: int, z: int} $update */
			$update = unpack("Cx/ly/Cz", $record, $offset);
			$offset += 6;
			$fluid[] = [$chunkX * 16 + $update["x"], $update["y"], $chunkZ * 16 + $update["z"]];
		}
		if($offset !== strlen($record)){
			fail($path . ": chunk (" . $chunkX . ", " . $chunkZ . ")'s record is " . strlen($record) . " bytes, " . $offset . " read");
		}
		return [[$chunkX, $chunkZ], ["subChunks" => $subChunks, "fluid" => $fluid]];
	}

	/**
	 * One PalettedLayer as the bench writes it, read at $offset and moved past.
	 *
	 * @return array{int, string, list<int>}
	 */
	private static function layer(string $record, int &$offset) : array{
		$bits = ord($record[$offset]);
		/** @var array{words: int} $words */
		$words = unpack("Vwords", $record, $offset + 1);
		$offset += 5;
		$wordBytes = substr($record, $offset, 4 * $words["words"]);
		$offset += 4 * $words["words"];
		/** @var array{entries: int} $entries */
		$entries = unpack("Ventries", $record, $offset);
		$offset += 4;
		$palette = [];
		if($entries["entries"] > 0){
			/** @var array<int, int> $values */
			$values = unpack("V" . $entries["entries"], $record, $offset);
			foreach($values as $value){
				$palette[] = $value;
			}
		}
		$offset += 4 * $entries["entries"];
		return [$bits, $wordBytes, $palette];
	}
}
