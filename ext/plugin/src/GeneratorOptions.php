<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\world\generator\InvalidGeneratorOptionsException;
use function array_key_exists;
use function is_array;
use function is_readable;
use function is_string;
use function json_decode;
use function json_encode;
use function json_last_error_msg;
use const JSON_THROW_ON_ERROR;

/**
 * What a Stratum world's generator options string says, and where the
 * frozen pipeline it generates from lives.
 *
 * The options string is the only configuration a generator ever receives:
 * PocketMine-MP constructs one per worker thread with nothing but the seed
 * and this string (GeneratorExecutorSetupParameters::createGenerator), and
 * it is stored in the world's own data. So the path to the frozen pipeline
 * (SPEC section 6) travels in here.
 *
 *   {"blob":"/path/worlds/myworld/stratum-pipeline.blob",
 *    "settings":"minecraft:overworld",
 *    "biomes":"minecraft:overworld"}
 *
 * "biomes" is the dimension's biome source: a multi-noise parameter list
 * ("minecraft:overworld", "minecraft:nether"), or "minecraft:the_end" for
 * the End's own. The End is {"settings":"minecraft:end",
 * "biomes":"minecraft:the_end"}.
 *
 * Both default to the overworld's, but only TOGETHER: options that name
 * "settings" must name "biomes" too. Noise settings do not say which biome
 * source goes with them, so "settings" alone would quietly pair whatever it
 * names with the overworld's biomes, and for the End that is a world that
 * generates, with every biome wrong.
 */
final class GeneratorOptions{

	private const DEFAULT_DIMENSION = "minecraft:overworld";

	private function __construct(
		public readonly string $blobPath,
		public readonly string $noiseSettings,
		public readonly string $biomeSource
	){}

	public static function encode(string $blobPath, string $noiseSettings = self::DEFAULT_DIMENSION, string $biomeSource = self::DEFAULT_DIMENSION) : string{
		return json_encode([
			"blob" => $blobPath,
			"settings" => $noiseSettings,
			"biomes" => $biomeSource
		], JSON_THROW_ON_ERROR);
	}

	/**
	 * @throws InvalidGeneratorOptionsException naming what is wrong, which
	 * PocketMine-MP reports as an invalid-options world load failure.
	 */
	public static function parse(string $generatorOptions) : self{
		if($generatorOptions === ""){
			throw new InvalidGeneratorOptionsException(
				"Stratum needs a frozen pipeline to generate from. Create the world with " .
				"Stratum\\PMMP\\WorldFactory::create(), or set its generator options to " .
				'{"blob":"<path to stratum-pipeline.blob>"}'
			);
		}
		$decoded = json_decode($generatorOptions, associative: true);
		if(!is_array($decoded)){
			throw new InvalidGeneratorOptionsException("Stratum's generator options are not a JSON object (" . json_last_error_msg() . "): " . $generatorOptions);
		}
		$blob = $decoded["blob"] ?? null;
		if(!is_string($blob) || $blob === ""){
			throw new InvalidGeneratorOptionsException('Stratum\'s generator options have no "blob" path to a frozen pipeline: ' . $generatorOptions);
		}
		// Checked here, on the main thread, where the failure can still be
		// reported against the world being loaded. A worker thread opening a
		// missing blob would only be able to take the generation task down.
		if(!is_readable($blob)){
			throw new InvalidGeneratorOptionsException("Stratum cannot read this world's frozen pipeline at " . $blob);
		}
		if(array_key_exists("settings", $decoded) && !array_key_exists("biomes", $decoded)){
			throw new InvalidGeneratorOptionsException('Stratum\'s generator options name "settings" without "biomes", the biome source to go with them ("minecraft:overworld", "minecraft:nether" or "minecraft:the_end"): ' . $generatorOptions);
		}
		$settings = $decoded["settings"] ?? self::DEFAULT_DIMENSION;
		$biomes = $decoded["biomes"] ?? self::DEFAULT_DIMENSION;
		if(!is_string($settings) || !is_string($biomes)){
			throw new InvalidGeneratorOptionsException('Stratum\'s "settings" and "biomes" options must be strings: ' . $generatorOptions);
		}
		return new self($blob, $settings, $biomes);
	}

	/** The shape GeneratorManager::addGenerator() wants: return, never throw. */
	public static function validate(string $generatorOptions) : ?InvalidGeneratorOptionsException{
		try{
			self::parse($generatorOptions);
			return null;
		}catch(InvalidGeneratorOptionsException $e){
			return $e;
		}
	}
}
