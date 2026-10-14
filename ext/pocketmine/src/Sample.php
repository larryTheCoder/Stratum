<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use function intdiv;
use function sys_get_temp_dir;
use function tempnam;

/**
 * The worlds the checks generate: a pipeline frozen from a version root, and
 * the chunks to sample from it. Fixed, so two runs sample the same terrain.
 */
final class Sample{

	/** The seed every check generates at, the one ext/zend/tests use. */
	public const SEED = 20260915;

	/**
	 * Every dimension this build ships: [noise settings, biome source]. The
	 * Nether is not here — its surface rules' named noises are still refused
	 * by name (SPEC section 10, M4).
	 */
	public const DIMENSIONS = [
		"overworld" => ["minecraft:overworld", "minecraft:overworld"],
		"end" => ["minecraft:end", "minecraft:the_end"]
	];

	private function __construct(){}

	/**
	 * Freezes $versionRoot into a new temporary file and returns its path;
	 * the caller unlinks it.
	 */
	public static function freeze(string $versionRoot) : string{
		$blob = tempnam(sys_get_temp_dir(), "stratum-pmmp-blob-");
		if($blob === false){
			fail("cannot create a temporary file for the frozen pipeline");
		}
		\Stratum\freezePipeline($versionRoot, $blob);
		return $blob;
	}

	/**
	 * A $side x $side grid of chunk coordinates, $spacing chunks apart and
	 * centred on the origin, so a sample spans terrain rather than one patch
	 * of it: at side 9 and spacing 41, about 5900 blocks across.
	 *
	 * @return list<array{int, int}>
	 */
	public static function grid(int $side, int $spacing) : array{
		$chunks = [];
		$half = intdiv($side, 2);
		for($i = -$half; $i < $side - $half; $i++){
			for($j = -$half; $j < $side - $half; $j++){
				$chunks[] = [$i * $spacing, $j * $spacing];
			}
		}
		return $chunks;
	}

	/**
	 * Every Java block state id and Bedrock biome id encodeChunk() put in a
	 * palette over $chunks.
	 *
	 * @param list<array{int, int}> $chunks
	 *
	 * @return array{array<int, true>, array<int, true>} [Java state ids, Bedrock biome ids]
	 */
	public static function palettes(\Stratum\Dimension $dimension, array $chunks) : array{
		$states = [];
		$biomes = [];
		foreach($chunks as [$chunkX, $chunkZ]){
			foreach($dimension->encodeChunk($chunkX, $chunkZ) as $layers){
				if($layers["blocks"] !== null){
					foreach($layers["blocks"][2] as $javaId){
						$states[$javaId] = true;
					}
				}
				foreach($layers["biomes"][2] as $biomeId){
					$biomes[$biomeId] = true;
				}
			}
		}
		return [$states, $biomes];
	}
}
