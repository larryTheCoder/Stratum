<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\math\Vector3;
use pocketmine\Server;
use pocketmine\world\generator\InvalidGeneratorOptionsException;
use pocketmine\world\WorldCreationOptions;
use Symfony\Component\Filesystem\Path;
use function array_key_exists;
use function array_keys;
use function implode;
use function is_dir;
use function mkdir;
use function trim;

/**
 * Creating a world that generates from a frozen pipeline (SPEC section 6).
 *
 * The order matters, and each step is there for a reason:
 *
 *  1. A dimension this does not know, a name that is empty or already a
 *     world, is refused before anything is written. An existing world's
 *     folder holds the frozen pipeline every chunk it has was generated
 *     from; freezing over it — from another pack, or another build — would
 *     change the terrain of every chunk generated from then on, a seam
 *     through the world (SPEC section 6), and generateWorld() refusing the
 *     name afterwards would not undo it.
 *  2. The blob is written before the world exists. PocketMine-MP's
 *     generateWorld() constructs the World immediately, and from then on
 *     generators may open it. An empty world directory does not count as a
 *     generated world (no provider matches it), so creating it early is
 *     safe; and Stratum\freezePipeline() writes whole or not at all.
 *  3. The options are checked in full here, on the main thread
 *     (GeneratorOptions::check), so a pipeline that cannot generate is
 *     refused to the caller rather than on a worker.
 *
 * The options name the blob RELATIVELY, worlds/<name>/stratum-pipeline.blob,
 * read against the server's data path (Stratum\setDataPath, which this and
 * Main give the extension): PocketMine-MP stores the options inside the
 * world and validates them at every load, and an absolute path stopped a
 * moved server directory from loading its worlds.
 */
final class WorldFactory{

	public const BLOB_NAME = "stratum-pipeline.blob";

	/** Vanilla's overworld: the overworld's noise settings and parameter list. */
	public const OVERWORLD = "overworld";
	/** Vanilla's large_biomes world preset's overworld. */
	public const LARGE_BIOMES = "large_biomes";
	/** Vanilla's amplified world preset's overworld. */
	public const AMPLIFIED = "amplified";
	/** Vanilla's End: the End's noise settings and its own biome source. */
	public const THE_END = "the_end";

	/**
	 * Each dimension create() makes => [noise settings, biome source], as
	 * vanilla's world presets pair them (worldgen/world_preset/*.json). The
	 * Nether is not here: its settings use the legacy random source, which
	 * this build refuses (SPEC section 8).
	 */
	private const DIMENSIONS = [
		self::OVERWORLD => ["minecraft:overworld", "minecraft:overworld"],
		self::LARGE_BIOMES => ["minecraft:large_biomes", "minecraft:overworld"],
		self::AMPLIFIED => ["minecraft:amplified", "minecraft:overworld"],
		self::THE_END => ["minecraft:end", "minecraft:the_end"],
	];

	/**
	 * Where an End world spawns: over the main island, which every End seed
	 * has at the origin, and above its terrain (y 0 to 127), so a player is
	 * put on top of it (World::getSafeSpawn searches down). PocketMine-MP's
	 * own default, (256, 70, 256), is over the void between the main island
	 * and the outer ones, and the spawn area it generates round it was all
	 * air.
	 */
	public const END_SPAWN = [0, 128, 0];

	/**
	 * Freezes the pipeline under $versionRoot into the new world's own
	 * folder and generates the world from it, as $dimension.
	 *
	 * $versionRoot is a version root as tools/fetch-vanilla leaves it:
	 * worldgen/ and biome_parameters/ side by side. $dimension is one of
	 * OVERWORLD, LARGE_BIOMES, AMPLIFIED or THE_END; THE_END's world spawns
	 * at END_SPAWN, over the main island.
	 *
	 * Returns false, writing nothing, when $worldName is empty or already a
	 * world — loaded or not — as WorldManager::generateWorld() would; that
	 * world's frozen pipeline is left exactly as it was.
	 *
	 * @throws \InvalidArgumentException for a $dimension this does not
	 * make, before anything is written.
	 * @throws \Stratum\GenerationException if the pipeline cannot be
	 * resolved or written.
	 * @throws InvalidGeneratorOptionsException if the frozen pipeline cannot
	 * generate the world (GeneratorOptions::check says what is refused).
	 */
	public static function create(Server $server, string $worldName, int $seed, string $versionRoot, string $dimension = self::OVERWORLD) : bool{
		if(!array_key_exists($dimension, self::DIMENSIONS)){
			throw new \InvalidArgumentException("WorldFactory::create() makes the dimensions " . implode(", ", array_keys(self::DIMENSIONS)) . ", not \"" . $dimension . "\"");
		}
		if(trim($worldName) === "" || $server->getWorldManager()->isWorldGenerated($worldName)){
			return false;
		}
		// What the relative path below is read against: the server's data
		// path, as Main has already set it. Setting the same path again
		// changes nothing.
		\Stratum\setDataPath($server->getDataPath());
		$worldPath = self::worldPath($server, $worldName);
		if(!is_dir($worldPath)){
			mkdir($worldPath, 0o777, recursive: true);
		}
		$blobPath = self::blobPath($worldName);
		\Stratum\freezePipeline($versionRoot, $blobPath);

		[$noiseSettings, $biomeSource] = self::DIMENSIONS[$dimension];
		$generatorOptions = GeneratorOptions::encode($blobPath, $noiseSettings, $biomeSource);
		GeneratorOptions::check($generatorOptions);

		$options = WorldCreationOptions::create()
			->setGeneratorClass(StratumGenerator::class)
			->setGeneratorOptions($generatorOptions)
			->setSeed($seed);
		if($dimension === self::THE_END){
			$options->setSpawnPosition(new Vector3(...self::END_SPAWN));
		}

		return $server->getWorldManager()->generateWorld($worldName, $options);
	}

	/**
	 * Where PocketMine-MP keeps a world. WorldManager computes this itself
	 * but keeps it private, so it is reproduced here rather than guessed at
	 * from a World instance that does not exist yet.
	 */
	public static function worldPath(Server $server, string $worldName) : string{
		return Path::join($server->getDataPath(), "worlds", $worldName);
	}

	/**
	 * A world's frozen pipeline, as its options name it: relative to the
	 * server's data path.
	 */
	public static function blobPath(string $worldName) : string{
		return Path::join("worlds", $worldName, self::BLOB_NAME);
	}
}
