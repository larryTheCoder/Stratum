<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\Server;
use pocketmine\world\generator\InvalidGeneratorOptionsException;
use pocketmine\world\WorldCreationOptions;
use Symfony\Component\Filesystem\Path;
use function is_dir;
use function mkdir;
use function trim;

/**
 * Creating a world that generates from a frozen pipeline (SPEC section 6).
 *
 * The order matters, and each step is there for a reason:
 *
 *  1. A name that is already a world is refused before anything is written.
 *     Its folder holds the frozen pipeline every chunk it has was generated
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
	 * Returns false, writing nothing, when $worldName is empty or already a
	 * world — loaded or not — as WorldManager::generateWorld() would; that
	 * world's frozen pipeline is left exactly as it was.
	 *
	 * @throws \Stratum\GenerationException if the pipeline cannot be
	 * resolved or written.
	 * @throws InvalidGeneratorOptionsException if the frozen pipeline cannot
	 * generate the world (GeneratorOptions::check says what is refused).
	 */
	public static function create(Server $server, string $worldName, int $seed, string $versionRoot) : bool{
		if(trim($worldName) === "" || $server->getWorldManager()->isWorldGenerated($worldName)){
			return false;
		}
		$worldPath = self::worldPath($server, $worldName);
		if(!is_dir($worldPath)){
			mkdir($worldPath, 0o777, recursive: true);
		}
		$blobPath = Path::join($worldPath, self::BLOB_NAME);
		\Stratum\freezePipeline($versionRoot, $blobPath);

		$generatorOptions = GeneratorOptions::encode($blobPath);
		GeneratorOptions::check($generatorOptions);

		$options = WorldCreationOptions::create()
			->setGeneratorClass(StratumGenerator::class)
			->setGeneratorOptions($generatorOptions)
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
