<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\world\generator\InvalidGeneratorOptionsException;
use function array_diff;
use function array_key_exists;
use function array_keys;
use function implode;
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
 * EXACTLY WHAT IS ACCEPTED: a JSON object with these keys and no others.
 *
 *   "blob"      required: the path of a readable file, the world's frozen
 *               pipeline. Whether it IS one is Stratum\Dimension::open's to
 *               say, on the worker.
 *   "settings"  the noise settings, by id ("minecraft:overworld",
 *               "minecraft:nether", "minecraft:end"), a string;
 *   "biomes"    the dimension's biome source, a string, as
 *               world::BiomeSource::parse reads it: a multi-noise parameter
 *               list's id ("minecraft:overworld", "minecraft:nether"), or
 *               "minecraft:the_end" for the End's own. A bare path is in
 *               the minecraft namespace ("the_end").
 *
 * "settings" and "biomes" come together or not at all; with neither, both
 * are the overworld's. Noise settings do not say which biome source goes
 * with them, nor a source which settings, so either alone would quietly be
 * paired with the overworld's other half — the End's settings with the
 * overworld's biomes, or the overworld's terrain with the End's — a world
 * that generates, with plausible biomes, all wrong.
 *
 * REFUSED HERE, on the main thread where a failure can still be reported
 * against the world being loaded, each with what is wrong: an empty string;
 * anything but a JSON object; no "blob", or one that cannot be read; a key
 * this list does not name (a misspelt "biome" is not quietly ignored);
 * "settings" without "biomes" or "biomes" without "settings"; either not a
 * string; and a "biomes" that world::BiomeSource::parse refuses —
 * "minecraft:fixed", "minecraft:checkerboard", "minecraft:multi_noise"
 * named with no list, text that is no identifier — with parse's own reason,
 * through the extension's Stratum\parseBiomeSource.
 *
 * REFUSED LATER, by name, when a worker opens the pipeline
 * (Stratum\Dimension::open): settings or a parameter list the pipeline does
 * not carry, and a dimension this build cannot compile.
 *
 * NOT REFUSED: a pairing vanilla does not make. A datapack may pair noise
 * settings with any biome source, so the options take the pair they are
 * given; vanilla's End is {"settings":"minecraft:end",
 * "biomes":"minecraft:the_end"}, and {"settings":"minecraft:end",
 * "biomes":"minecraft:overworld"} generates the End's terrain with the
 * overworld's biomes, as asked.
 */
final class GeneratorOptions{

	private const DEFAULT_DIMENSION = "minecraft:overworld";

	/** Every key the options may hold; any other is refused. */
	private const KEYS = ["blob", "settings", "biomes"];

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
		$unknown = array_diff(array_keys($decoded), self::KEYS);
		if($unknown !== []){
			throw new InvalidGeneratorOptionsException('Stratum\'s generator options take "blob", "settings" and "biomes" and nothing else, so "' . implode('", "', $unknown) . '" would be ignored: ' . $generatorOptions);
		}
		$hasSettings = array_key_exists("settings", $decoded);
		if($hasSettings !== array_key_exists("biomes", $decoded)){
			throw new InvalidGeneratorOptionsException($hasSettings ?
				'Stratum\'s generator options name "settings" without "biomes", the biome source to go with them ("minecraft:overworld", "minecraft:nether" or "minecraft:the_end"): ' . $generatorOptions :
				'Stratum\'s generator options name "biomes" without "settings", the noise settings to go with them ("minecraft:overworld", "minecraft:nether" or "minecraft:end"): ' . $generatorOptions
			);
		}
		$settings = $decoded["settings"] ?? self::DEFAULT_DIMENSION;
		$biomes = $decoded["biomes"] ?? self::DEFAULT_DIMENSION;
		if(!is_string($settings) || !is_string($biomes)){
			throw new InvalidGeneratorOptionsException('Stratum\'s "settings" and "biomes" options must be strings: ' . $generatorOptions);
		}
		// The extension's own parse, so this refuses exactly what
		// Dimension::open would and says why in its words — here, rather
		// than on a worker thread that could only take the task down.
		try{
			\Stratum\parseBiomeSource($biomes);
		}catch(\Stratum\GenerationException $e){
			throw new InvalidGeneratorOptionsException('Stratum cannot generate from the "biomes" option: ' . $e->getMessage() . ': ' . $generatorOptions);
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
