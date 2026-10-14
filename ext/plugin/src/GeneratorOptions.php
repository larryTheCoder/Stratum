<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\world\generator\InvalidGeneratorOptionsException;
use function array_diff;
use function array_key_exists;
use function array_keys;
use function implode;
use function is_array;
use function is_file;
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
 *   {"blob":"worlds/myworld/stratum-pipeline.blob",
 *    "settings":"minecraft:overworld",
 *    "biomes":"minecraft:overworld"}
 *
 * EXACTLY WHAT IS ACCEPTED: a JSON object with these keys and no others.
 *
 *   "blob"      required: the path of the world's frozen pipeline, a
 *               readable file that Stratum\checkDimension opens. Relative
 *               to the server's data path, as WorldFactory writes it, so
 *               the world moves with its server directory; an absolute
 *               path, as worlds made before that have, is read as it is
 *               and pins the world to where it was made.
 *   "settings"  the noise settings, by id ("minecraft:overworld",
 *               "minecraft:nether", "minecraft:end"), a string;
 *   "biomes"    the dimension's biome source, a string, as
 *               world::BiomeSource::parse reads it: a multi-noise parameter
 *               list's id ("minecraft:overworld", "minecraft:nether"), or
 *               "minecraft:the_end" for the End's own. A bare path is in
 *               the minecraft namespace ("the_end").
 *
 * "settings" and "biomes" come together or not at all; with neither key,
 * both are the overworld's. A key that is present counts as given, null
 * included: {"settings":null,...} is refused as not a string, never read as
 * absent. Noise settings do not say which biome source goes with them, nor
 * a source which settings, so either alone would quietly be paired with the
 * overworld's other half — the End's settings with the overworld's biomes,
 * or the overworld's terrain with the End's — a world that generates, with
 * plausible biomes, all wrong.
 *
 * EVERY REFUSAL IS MADE ON THE MAIN THREAD, by validate(), which is what
 * PocketMine-MP calls when it loads (or creates) the world: there a failure
 * is reported against the world being loaded, while on a generation worker
 * an exception takes the whole server down. Each says what is wrong.
 *
 * parse() reads the string alone, and is what a worker's generator calls:
 * an empty string; anything but a JSON object; no "blob"; a key this list
 * does not name (a misspelt "biome" is not quietly ignored); "settings"
 * without "biomes" or "biomes" without "settings"; either not a string
 * (null, a number, an object); and a "biomes" that world::BiomeSource::parse
 * refuses — "minecraft:fixed", "minecraft:checkerboard",
 * "minecraft:multi_noise" named with no list, text that is no identifier —
 * with parse's own reason, through the extension's Stratum\parseBiomeSource.
 * It never touches the file: a pinned world's generators do not need it
 * (StratumWorlds).
 *
 * check(), which validate() wraps, is parse() and then the whole of opening
 * the world, through Stratum\checkDimension, with nothing kept: a "blob"
 * that cannot be read or is no frozen pipeline; one frozen by another
 * pipeline engine version, container format or Minecraft pin, or that fails
 * its own content hash (SPEC section 6); "settings" that are no identifier,
 * settings or a parameter list the pipeline does not carry, a
 * legacy-random-source dimension (vanilla's Nether), a dimension this build
 * cannot compile; a dimension outside PocketMine-MP's 24 sub-chunks, a
 * parameter list naming a biome with no Bedrock id, or a block state the
 * pack names — the settings' default block or fluid, a surface rule's
 * result — that the mapping table cannot place.
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

	/**
	 * The options for a world generated from $blobPath: the overworld when
	 * neither $noiseSettings nor $biomeSource is given, otherwise the pair as
	 * given. One without the other throws, as parse() refuses it: defaulting
	 * the missing half to the overworld's would turn encode($blob,
	 * "minecraft:end") into the End's terrain under the overworld's biomes —
	 * a world that generates and is wrong (SPEC §8).
	 *
	 * @throws \InvalidArgumentException naming the missing half.
	 */
	public static function encode(string $blobPath, ?string $noiseSettings = null, ?string $biomeSource = null) : string{
		if(($noiseSettings === null) !== ($biomeSource === null)){
			throw new \InvalidArgumentException($noiseSettings === null ?
				"GeneratorOptions::encode() was given a biome source (" . $biomeSource . ") without the noise settings to go with it" :
				"GeneratorOptions::encode() was given noise settings (" . $noiseSettings . ") without the biome source to go with them"
			);
		}
		return json_encode([
			"blob" => $blobPath,
			"settings" => $noiseSettings ?? self::DEFAULT_DIMENSION,
			"biomes" => $biomeSource ?? self::DEFAULT_DIMENSION
		], JSON_THROW_ON_ERROR);
	}

	/**
	 * The options string alone: nothing is read from disk and nothing is
	 * opened, so a worker's generator can call it.
	 *
	 * @throws InvalidGeneratorOptionsException naming what is wrong.
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
		// Read through the key test alone, never `??`: a JSON null is a key
		// that is present, and must reach is_string to be refused rather
		// than quietly become the overworld's half of the pair.
		$settings = $hasSettings ? $decoded["settings"] : self::DEFAULT_DIMENSION;
		$biomes = $hasSettings ? $decoded["biomes"] : self::DEFAULT_DIMENSION;
		if(!is_string($settings) || !is_string($biomes)){
			throw new InvalidGeneratorOptionsException('Stratum\'s "settings" and "biomes" options must be strings: ' . $generatorOptions);
		}
		// The extension's own parse, so this refuses exactly what
		// Dimension::open would and says why in its words.
		try{
			\Stratum\parseBiomeSource($biomes);
		}catch(\Stratum\GenerationException $e){
			throw new InvalidGeneratorOptionsException('Stratum cannot generate from the "biomes" option: ' . $e->getMessage() . ': ' . $generatorOptions);
		}
		return new self($blob, $settings, $biomes);
	}

	/**
	 * parse(), and then everything opening the world would refuse
	 * (Stratum\checkDimension), refused here. For the main thread: it reads
	 * and compiles the world's whole frozen pipeline, and keeps nothing.
	 *
	 * @throws InvalidGeneratorOptionsException naming what is wrong, which
	 * PocketMine-MP reports as an invalid-options world load failure.
	 */
	public static function check(string $generatorOptions) : self{
		$options = self::parse($generatorOptions);
		// Where the extension reads it: a relative path against the server's
		// data path, never PHP's working directory.
		try{
			$blobPath = \Stratum\resolvePipelinePath($options->blobPath);
		}catch(\Stratum\GenerationException $e){
			throw new InvalidGeneratorOptionsException("Stratum cannot find this world's frozen pipeline: " . $e->getMessage());
		}
		// is_file as well: is_readable is true of a directory.
		if(!is_file($blobPath) || !is_readable($blobPath)){
			throw new InvalidGeneratorOptionsException("Stratum cannot read this world's frozen pipeline at " . $blobPath);
		}
		try{
			\Stratum\checkDimension($options->blobPath, $options->noiseSettings, $options->biomeSource);
		}catch(\Stratum\GenerationException $e){
			throw new InvalidGeneratorOptionsException("Stratum cannot generate this world from its frozen pipeline at " . $blobPath . ": " . $e->getMessage());
		}
		return $options;
	}

	/**
	 * check(), in the shape GeneratorManager::addGenerator() wants: return,
	 * never throw. PocketMine-MP calls it on the main thread when it loads or
	 * creates a world with this generator.
	 */
	public static function validate(string $generatorOptions) : ?InvalidGeneratorOptionsException{
		try{
			self::check($generatorOptions);
			return null;
		}catch(InvalidGeneratorOptionsException $e){
			return $e;
		}
	}
}
