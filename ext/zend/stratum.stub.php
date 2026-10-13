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
 * What world::BiomeSource::parse reads $biomeSource as, spelled back in
 * full: "minecraft:the_end" for the End's own source, or the multi-noise
 * parameter list's id ("overworld" comes back "minecraft:overworld"). Opens
 * and compiles nothing, so a caller can refuse a source before any worker
 * opens a Dimension with it.
 *
 * @throws GenerationException with parse's reason, for the types this build
 * does not implement ("minecraft:fixed", "minecraft:checkerboard"), for
 * "minecraft:multi_noise" named with no list, and for text that is no
 * identifier. Whether the frozen pipeline carries a list is Dimension::open's
 * to say.
 */
function parseBiomeSource(string $biomeSource) : string{}

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
function takeFluidUpdates(string $blobPath, string $noiseSettings, string $biomeSource, int $seed, int $chunkX, int $chunkZ) : array{}

final class Dimension{
	private function __construct(){}

	/**
	 * One dimension of a world, compiled from its frozen pipeline. Shared by
	 * every thread that opens the same blob, settings, biome source and seed.
	 *
	 * $biomeSource names the dimension's biome source: a multi-noise
	 * parameter list's id ("minecraft:overworld", "minecraft:nether"), or
	 * "minecraft:the_end" for the End's own. "minecraft:fixed" and
	 * "minecraft:checkerboard" are refused by name.
	 */
	public static function open(string $blobPath, string $noiseSettings, string $biomeSource, int $seed) : Dimension{}

	public function getMinY() : int{}

	public function getHeight() : int{}

	/**
	 * All 24 of PocketMine-MP's sub-chunks, keyed by sub-chunk index (-4 to
	 * 19), whatever the dimension's height: those outside it are air, with
	 * the dimension's biomes read at their own y, so a short dimension such
	 * as the End is never left to Chunk's all-ocean default. Each layer is
	 * PalettedBlockArray::fromData()'s argument list. Block palettes hold
	 * Java block state ids; biome palettes hold Bedrock ids.
	 * The chunk's fluid updates are left for takeFluidUpdates(), replacing
	 * any an earlier encoding of the same chunk left.
	 *
	 * @return array<int, array{blocks: array{int, string, list<int>}|null, biomes: array{int, string, list<int>}}>
	 */
	public function encodeChunk(int $chunkX, int $chunkZ) : array{}
}

final class GenerationException extends \RuntimeException{}
