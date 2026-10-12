<?php

/**
 * Stratum's PocketMine-MP module, as PHP sees it. Documentation for IDEs and
 * static analysis only: the implementation is ext/zend/php_stratum.cpp.
 *
 * @generate-function-entries false
 */

namespace Stratum;

/**
 * Freezes the pipeline under $versionRoot (worldgen/ and biome_parameters/
 * side by side, as tools/fetch-vanilla leaves them) into $blobPath (SPEC §6).
 * A world generates from that blob and never from the pack again.
 */
function freezePipeline(string $versionRoot, string $blobPath) : void{}

/**
 * The Bedrock blockstate for vanilla Java block state id $javaStateId.
 * `states` maps each state name to [NBT tag id (1 byte, 3 int, 8 string), value].
 *
 * @return array{name: string, states: array<string, array{int, int|string}>, version: int}
 */
function bedrockBlockState(int $javaStateId) : array{}

function javaBlockStateCount() : int{}

/**
 * Removes and returns the fluid updates Dimension::encodeChunk() left for
 * chunk ($chunkX, $chunkZ) of the world whose generator was opened with
 * these options and this seed, as world [x, y, z] positions. These are the
 * positions a Java server keeps in the chunk's PostProcessing lists and ticks
 * once the chunk loads (spec Q8.1), which is what makes aquifer water and
 * lava flow; PocketMine-MP has no such list, so the caller schedules a block
 * update at each. Built for the main thread: it opens and compiles nothing,
 * and returns [] when no Dimension is open for that world or nothing is held
 * for the chunk. Each chunk's updates are returned once.
 *
 * @return list<array{int, int, int}>
 */
function takeFluidUpdates(string $blobPath, string $noiseSettings, string $biomeParameterList, int $seed, int $chunkX, int $chunkZ) : array{}

final class Dimension{
	private function __construct(){}

	/**
	 * One dimension of a world, compiled from its frozen pipeline. Shared by
	 * every thread that opens the same blob, settings, list and seed.
	 */
	public static function open(string $blobPath, string $noiseSettings, string $biomeParameterList, int $seed) : Dimension{}

	public function getMinY() : int{}

	public function getHeight() : int{}

	/**
	 * Every sub-chunk of the chunk, keyed by PocketMine-MP sub-chunk index.
	 * Each layer is PalettedBlockArray::fromData()'s argument list. Block
	 * palettes hold Java block state ids; biome palettes hold Bedrock ids.
	 * The chunk's fluid updates are left for takeFluidUpdates(), replacing
	 * any an earlier encoding of the same chunk left.
	 *
	 * @return array<int, array{blocks: array{int, string, list<int>}|null, biomes: array{int, string, list<int>}}>
	 */
	public function encodeChunk(int $chunkX, int $chunkZ) : array{}
}

final class GenerationException extends \RuntimeException{}
