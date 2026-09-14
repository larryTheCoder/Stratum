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
	 *
	 * @return array<int, array{blocks: array{int, string, list<int>}|null, biomes: array{int, string, list<int>}}>
	 */
	public function encodeChunk(int $chunkX, int $chunkZ) : array{}
}

final class GenerationException extends \RuntimeException{}
