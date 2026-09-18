<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\Server;
use pocketmine\world\WorldCreationOptions;
use Symfony\Component\Filesystem\Path;
use function is_dir;
use function mkdir;

/**
 * Creating a world that generates from a frozen pipeline (SPEC section 6).
 *
 * The order matters. PocketMine-MP's generateWorld() constructs the World
 * immediately, which registers the generator on a worker thread, which opens
 * the blob — so the blob has to be on disk first. An empty world directory
 * does not count as a generated world (no provider matches it), so creating
 * it early is safe.
 */
final class WorldFactory{

	public const BLOB_NAME = "stratum-pipeline.blob";

	/**
	 * Freezes the pipeline under $versionRoot into the new world's own
	 * folder and generates the world from it.
	 *
	 * $versionRoot is a version root as tools/fetch-vanilla leaves it:
	 * worldgen/ and biome_parameters/ side by side.
	 *
	 * @throws \Stratum\GenerationException if the pipeline cannot be resolved
	 * or written.
	 */
	public static function create(Server $server, string $worldName, int $seed, string $versionRoot) : bool{
		$worldPath = self::worldPath($server, $worldName);
		if(!is_dir($worldPath)){
			mkdir($worldPath, 0o777, recursive: true);
		}
		$blobPath = Path::join($worldPath, self::BLOB_NAME);
		\Stratum\freezePipeline($versionRoot, $blobPath);

		$options = WorldCreationOptions::create()
			->setGeneratorClass(StratumGenerator::class)
			->setGeneratorOptions(GeneratorOptions::encode($blobPath))
			->setSeed($seed);

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
}
