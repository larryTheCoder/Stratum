--TEST--
WorldFactory refuses an existing world before writing anything, and makes each dimension with its options, a relative blob path, and the End's spawn over its island
--EXTENSIONS--
stratum
--SKIPIF--
<?php
$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
if(!is_dir("$root/worldgen/noise_settings") || !is_dir("$root/biome_parameters")){
	die("skip no vanilla fixtures; run tools/fetch-vanilla");
}
?>
--FILE--
<?php
// create() used to freeze into the world's folder first and let
// generateWorld() refuse an existing name afterwards — by which time an
// existing world's frozen pipeline had been overwritten, and every chunk it
// generated from then on came from another pipeline: a seam (SPEC §6). It
// also only ever made the overworld, named its blob by an absolute path (a
// world that could not move), and left an End world PocketMine-MP's default
// spawn, over the void.
//
// Stand-ins, with the signatures WorldFactory calls, read from
// PocketMine-MP 5.44.3's phar: Server::getDataPath (absolute, with a
// trailing separator) /getWorldManager, WorldManager::isWorldGenerated
// (string)/generateWorld(string, WorldCreationOptions),
// WorldCreationOptions::create()->setGeneratorClass/setGeneratorOptions/
// setSeed/setSpawnPosition (a new one's spawn is (256, 70, 256)), Vector3,
// and Symfony's Path::join. The extension is real: the pipeline is frozen
// and checked by it.

namespace pocketmine\world\generator {
	class InvalidGeneratorOptionsException extends \UnexpectedValueException{}
}

namespace pocketmine\math {
	class Vector3{
		public function __construct(public int|float $x, public int|float $y, public int|float $z){}
	}
}

namespace Symfony\Component\Filesystem {
	final class Path{
		public static function join(string ...$paths) : string{
			$parts = [];
			foreach($paths as $i => $path){
				$parts[] = $i === 0 ? rtrim($path, "/") : trim($path, "/");
			}
			return implode("/", $parts);
		}
	}
}

namespace pocketmine\world {
	use pocketmine\math\Vector3;

	final class WorldCreationOptions{
		public string $generatorClass = "";
		public string $generatorOptions = "";
		public int $seed = 0;
		public Vector3 $spawnPosition;
		public function __construct(){ $this->spawnPosition = new Vector3(256, 70, 256); }
		public static function create() : self{ return new self(); }
		public function setGeneratorClass(string $class) : self{ $this->generatorClass = $class; return $this; }
		public function setGeneratorOptions(string $options) : self{ $this->generatorOptions = $options; return $this; }
		public function setSeed(int $seed) : self{ $this->seed = $seed; return $this; }
		public function setSpawnPosition(Vector3 $spawnPosition) : self{ $this->spawnPosition = $spawnPosition; return $this; }
	}

	final class WorldManager{
		/** @var list<array{string, WorldCreationOptions, bool}> name, options, whether the blob opened when it was called */
		public array $generated = [];

		/** @param list<string> $existing */
		public function __construct(private array $existing){}

		public function isWorldGenerated(string $name) : bool{
			return in_array($name, $this->existing, true);
		}

		public function generateWorld(string $name, WorldCreationOptions $options, bool $backgroundGeneration = true) : bool{
			if(trim($name) === "" || $this->isWorldGenerated($name)){
				return false;
			}
			// The World is constructed here, and generators may open the
			// blob from now on: it has to be complete already, and open as
			// the options name it — relative to the data path.
			$given = json_decode($options->generatorOptions, true);
			try{
				\Stratum\checkDimension($given["blob"], $given["settings"], $given["biomes"]);
				$opened = true;
			}catch(\Stratum\GenerationException){
				$opened = false;
			}
			$this->generated[] = [$name, $options, $opened];
			$this->existing[] = $name;
			return true;
		}
	}
}

namespace pocketmine {
	final class Server{
		public function __construct(private string $dataPath, private world\WorldManager $worldManager){}
		public function getDataPath() : string{ return $this->dataPath; }
		public function getWorldManager() : world\WorldManager{ return $this->worldManager; }
	}
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";
	require __DIR__ . "/../src/WorldFactory.php";

	use pocketmine\Server;
	use pocketmine\world\WorldManager;
	use Stratum\PMMP\WorldFactory;

	$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
	$dataPath = sys_get_temp_dir() . "/stratum-factory-" . getmypid();
	@mkdir("$dataPath/worlds/existing", 0o777, true);

	// An existing world, generated from a pipeline frozen by some earlier
	// build: its blob is whatever it is, and must stay so.
	$existingBlob = "$dataPath/worlds/existing/" . WorldFactory::BLOB_NAME;
	file_put_contents($existingBlob, "frozen by an earlier build; every chunk on disk came from this");
	$before = md5_file($existingBlob);

	$manager = new WorldManager(["existing"]);
	$server = new Server($dataPath . "/", $manager);

	// Refused, before anything is written: the blob is byte-identical, no
	// file appears beside it, and generateWorld() is never reached.
	var_dump(WorldFactory::create($server, "existing", 1, $root));
	var_dump(md5_file($existingBlob) === $before);
	echo "beside it: ", implode(",", array_diff(scandir("$dataPath/worlds/existing"), [".", ".."])), "\n";
	echo "generateWorld calls: ", count($manager->generated), "\n";

	// An empty name is no world, and a dimension it does not make is
	// refused by name: nothing is written under worlds/ for either.
	var_dump(WorldFactory::create($server, " ", 1, $root));
	try{
		WorldFactory::create($server, "nether", 1, $root, "the_nether");
		echo "no exception\n";
	}catch(\InvalidArgumentException $e){
		echo "refused: ", $e->getMessage(), "\n";
	}
	echo "worlds: ", implode(",", array_diff(scandir("$dataPath/worlds"), [".", ".."])), "\n";

	// A new world of each dimension: the blob is complete before the world
	// is constructed, the options name it relative to the data path, with
	// the dimension's settings and biome source, and only the End's spawn
	// is moved, over its main island.
	foreach([
		"fresh" => null,
		"large" => WorldFactory::LARGE_BIOMES,
		"amplified" => WorldFactory::AMPLIFIED,
		"end" => WorldFactory::THE_END,
	] as $name => $dimension){
		$created = $dimension === null ?
			WorldFactory::create($server, $name, 20260915, $root) :
			WorldFactory::create($server, $name, 20260915, $root, $dimension);
		[$generated, $options, $opened] = $manager->generated[count($manager->generated) - 1];
		$given = json_decode($options->generatorOptions, true);
		$spawn = $options->spawnPosition;
		echo $name, ": ", var_export($created, true), ", generated ", $generated, ", seed ", $options->seed,
			", blob ", $given["blob"], " (opened: ", var_export($opened, true), "), ",
			$given["settings"], " / ", $given["biomes"], ", spawn ", $spawn->x, " ", $spawn->y, " ", $spawn->z, "\n";
		// Written whole: no partial file is left beside it.
		echo "  beside it: ", implode(",", array_diff(scandir("$dataPath/worlds/$name"), [".", ".."])), "\n";
	}
	echo "class: ", $manager->generated[0][1]->generatorClass, "\n";

	// Created once, it is an existing world too: a second create() leaves it
	// alone.
	$freshBlob = "$dataPath/worlds/fresh/" . WorldFactory::BLOB_NAME;
	$frozen = md5_file($freshBlob);
	var_dump(WorldFactory::create($server, "fresh", 1, "/nonexistent/version-root"));
	var_dump(md5_file($freshBlob) === $frozen);

	// A pipeline that cannot be resolved is refused before anything is
	// written into the new world's folder.
	try{
		WorldFactory::create($server, "broken", 1, "/nonexistent/version-root");
		echo "no exception\n";
	}catch(\Stratum\GenerationException $e){
		echo "refused: ", get_class($e), "\n";
	}
	echo "broken holds: ", count(array_diff(scandir("$dataPath/worlds/broken"), [".", ".."])), " files\n";
	echo "generateWorld calls: ", count($manager->generated), "\n";

	foreach(["existing", "fresh", "large", "amplified", "end", "broken"] as $world){
		@unlink("$dataPath/worlds/$world/" . WorldFactory::BLOB_NAME);
		rmdir("$dataPath/worlds/$world");
	}
	rmdir("$dataPath/worlds");
	rmdir($dataPath);
}
?>
--EXPECT--
bool(false)
bool(true)
beside it: stratum-pipeline.blob
generateWorld calls: 0
bool(false)
refused: WorldFactory::create() makes the dimensions overworld, large_biomes, amplified, the_end, not "the_nether"
worlds: existing
fresh: true, generated fresh, seed 20260915, blob worlds/fresh/stratum-pipeline.blob (opened: true), minecraft:overworld / minecraft:overworld, spawn 256 70 256
  beside it: stratum-pipeline.blob
large: true, generated large, seed 20260915, blob worlds/large/stratum-pipeline.blob (opened: true), minecraft:large_biomes / minecraft:overworld, spawn 256 70 256
  beside it: stratum-pipeline.blob
amplified: true, generated amplified, seed 20260915, blob worlds/amplified/stratum-pipeline.blob (opened: true), minecraft:amplified / minecraft:overworld, spawn 256 70 256
  beside it: stratum-pipeline.blob
end: true, generated end, seed 20260915, blob worlds/end/stratum-pipeline.blob (opened: true), minecraft:end / minecraft:the_end, spawn 0 128 0
  beside it: stratum-pipeline.blob
class: Stratum\PMMP\StratumGenerator
bool(false)
bool(true)
refused: Stratum\GenerationException
broken holds: 0 files
generateWorld calls: 4
