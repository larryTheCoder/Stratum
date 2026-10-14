--TEST--
WorldFactory refuses an existing world before writing anything, so its frozen pipeline is left byte for byte
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
// generated from then on came from another pipeline: a seam (SPEC §6).
//
// Stand-ins, with the signatures WorldFactory calls, read from
// PocketMine-MP 5.44.4: Server::getDataPath/getWorldManager,
// WorldManager::isWorldGenerated(string)/generateWorld(string,
// WorldCreationOptions), WorldCreationOptions::create()->setGeneratorClass/
// setGeneratorOptions/setSeed, and Symfony's Path::join. The extension is
// real: the pipeline is frozen and checked by it.

namespace pocketmine\world\generator {
	class InvalidGeneratorOptionsException extends \UnexpectedValueException{}
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
	final class WorldCreationOptions{
		public string $generatorClass = "";
		public string $generatorOptions = "";
		public int $seed = 0;
		public static function create() : self{ return new self(); }
		public function setGeneratorClass(string $class) : self{ $this->generatorClass = $class; return $this; }
		public function setGeneratorOptions(string $options) : self{ $this->generatorOptions = $options; return $this; }
		public function setSeed(int $seed) : self{ $this->seed = $seed; return $this; }
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
			// blob from now on: it has to be complete already.
			$blob = json_decode($options->generatorOptions, true)["blob"];
			try{
				\Stratum\checkDimension($blob, "minecraft:overworld", "minecraft:overworld");
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
	$server = new Server($dataPath, $manager);

	// Refused, before anything is written: the blob is byte-identical, no
	// file appears beside it, and generateWorld() is never reached.
	var_dump(WorldFactory::create($server, "existing", 1, $root));
	var_dump(md5_file($existingBlob) === $before);
	echo "beside it: ", implode(",", array_diff(scandir("$dataPath/worlds/existing"), [".", ".."])), "\n";
	echo "generateWorld calls: ", count($manager->generated), "\n";

	// An empty name is no world: nothing is written under worlds/.
	var_dump(WorldFactory::create($server, " ", 1, $root));
	echo "worlds: ", implode(",", array_diff(scandir("$dataPath/worlds"), [".", ".."])), "\n";

	// A new world: the blob is complete before the world is constructed, and
	// the options name it.
	var_dump(WorldFactory::create($server, "fresh", 20260915, $root));
	[$name, $options, $opened] = $manager->generated[0];
	$freshBlob = "$dataPath/worlds/fresh/" . WorldFactory::BLOB_NAME;
	echo "generated: $name, seed ", $options->seed, ", class ", $options->generatorClass, ", blob opened: ", var_export($opened, true), "\n";
	var_dump(json_decode($options->generatorOptions, true)["blob"] === $freshBlob);
	// Written whole: no partial file is left beside it.
	echo "beside it: ", implode(",", array_diff(scandir("$dataPath/worlds/fresh"), [".", ".."])), "\n";

	// Created once, it is an existing world too: a second create() leaves it
	// alone.
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

	foreach(["existing", "fresh", "broken"] as $world){
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
worlds: existing
bool(true)
generated: fresh, seed 20260915, class Stratum\PMMP\StratumGenerator, blob opened: true
bool(true)
beside it: stratum-pipeline.blob
bool(false)
bool(true)
refused: Stratum\GenerationException
broken holds: 0 files
generateWorld calls: 1
