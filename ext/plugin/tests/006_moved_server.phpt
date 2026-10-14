--TEST--
A world WorldFactory made still loads, validates, pins and generates the same terrain once its server directory has moved
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
// A world's generator options are stored inside the world, and
// PocketMine-MP validates them at every load. They used to name the frozen
// pipeline by its absolute path, so a server directory moved or renamed
// refused its worlds — and stopped at startup when the default world was
// one. WorldFactory now names the blob relative to the server's data path,
// which Main hands the extension as it loads (Stratum\setDataPath). Here the
// server directory moves between two "runs" of the server: each run is what
// Main::onLoad does (setDataPath with the server's data path), what
// PocketMine-MP does as it loads the world (the validator Main registers,
// then WorldLoadEvent to StratumWorlds), and what a generation worker does
// (StratumGenerator's constructor: GeneratorOptions::parse, then
// Stratum\Dimension::open), then the world's unload.
//
// Stand-ins as in 004_world_pins.phpt and 005_world_factory.phpt, read from
// PocketMine-MP 5.44.3's phar. The extension is real.

namespace {
	interface Logger{
		public function error($message);
	}
}

namespace pocketmine\math {
	class Vector3{
		public function __construct(public int|float $x, public int|float $y, public int|float $z){}
	}
}

namespace pocketmine\event {
	interface Listener{}
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

namespace pocketmine\world\generator {
	class InvalidGeneratorOptionsException extends \UnexpectedValueException{}
	final class GeneratorManagerEntry{
		public function __construct(private string $generatorClass){}
		public function getGeneratorClass() : string{ return $this->generatorClass; }
	}
	final class GeneratorManager{
		private static ?self $instance = null;
		public static function getInstance() : self{ return self::$instance ??= new self(); }
		public function getGenerator(string $name) : ?GeneratorManagerEntry{
			return $name === "stratum" ? new GeneratorManagerEntry("Stratum\\PMMP\\StratumGenerator") : null;
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

	/** What WorldFactory hands generateWorld(), kept as the world's data. */
	final class WorldManager{
		/** @var array<string, WorldCreationOptions> */
		public array $created = [];
		public function isWorldGenerated(string $name) : bool{ return isset($this->created[$name]); }
		public function generateWorld(string $name, WorldCreationOptions $options, bool $backgroundGeneration = true) : bool{
			$this->created[$name] = $options;
			return true;
		}
	}

	class World{
		public function __construct(
			private int $id,
			private string $generatorOptions,
			private int $seed
		){}
		public function getId() : int{ return $this->id; }
		public function getSeed() : int{ return $this->seed; }
		public function getFolderName() : string{ return "w"; }
		public function getProvider() : object{
			$data = new class($this->generatorOptions){
				public function __construct(private string $options){}
				public function getGenerator() : string{ return "stratum"; }
				public function getGeneratorOptions() : string{ return $this->options; }
			};
			return new class($data){
				public function __construct(private object $data){}
				public function getWorldData() : object{ return $this->data; }
			};
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

namespace pocketmine\event\world {
	use pocketmine\world\World;

	class WorldLoadEvent{
		public function __construct(private World $world){}
		public function getWorld() : World{ return $this->world; }
	}
	class WorldUnloadEvent{
		public function __construct(private World $world){}
		public function getWorld() : World{ return $this->world; }
	}
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";
	require __DIR__ . "/../src/StratumWorlds.php";
	require __DIR__ . "/../src/WorldFactory.php";

	use pocketmine\event\world\WorldLoadEvent;
	use pocketmine\event\world\WorldUnloadEvent;
	use pocketmine\Server;
	use pocketmine\world\World;
	use pocketmine\world\WorldManager;
	use Stratum\PMMP\GeneratorOptions;
	use Stratum\PMMP\StratumWorlds;
	use Stratum\PMMP\WorldFactory;

	$logged = [];
	$logger = new class($logged) implements Logger{
		public function __construct(private array &$logged){}
		public function error($message){ $this->logged[] = $message; }
	};

	/**
	 * One run of the server whose directory is $dataPath, loading the world
	 * whose stored options are $options: what Main::onLoad, PocketMine-MP's
	 * world load, a generation worker and the world's unload do. Chunk
	 * (-3, 2)'s encoding, or why the world did not load.
	 */
	function run(string $dataPath, string $options, int $seed, Logger $logger) : string{
		Stratum\setDataPath($dataPath);
		$refused = GeneratorOptions::validate($options);
		if($refused !== null){
			return "refused at load: " . $refused->getMessage();
		}
		$world = new World(1, $options, $seed);
		$worlds = new StratumWorlds($logger);
		$worlds->onWorldLoad(new WorldLoadEvent($world));
		if(!$worlds->isPinned($world)){
			return "not pinned";
		}
		$parsed = GeneratorOptions::parse($options);
		$encoded = Stratum\Dimension::open($parsed->blobPath, $parsed->noiseSettings, $parsed->biomeSource, $seed)->encodeChunk(-3, 2);
		$worlds->onWorldUnload(new WorldUnloadEvent($world));
		return md5(serialize($encoded));
	}

	$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
	$base = sys_get_temp_dir() . "/stratum-moved-" . getmypid();
	$first = "$base/server";
	$second = "$base/elsewhere/renamed";
	@mkdir($first, 0o777, true);
	@mkdir("$base/elsewhere", 0o777, true);
	$seed = 20260915;

	// The first run creates the world, as a command or another plugin does.
	Stratum\setDataPath("$first/");
	$manager = new WorldManager();
	var_dump(WorldFactory::create(new Server("$first/", $manager), "w", $seed, $root));
	$options = $manager->created["w"]->generatorOptions;
	echo "stored options: ", $options, "\n";
	$terrain = run("$first/", $options, $seed, $logger);

	// The whole server directory moves, and the next run is handed its new
	// data path: the world loads, pins, and generates the same chunk.
	rename($first, $second);
	echo "after the move: ", run("$second/", $options, $seed, $logger) === $terrain ? "the same terrain" : "different", "\n";

	// What an older world's options hold, the absolute path of where it was
	// made, refuses the moved world at load, by name; the absolute path of
	// where it now is still opens.
	echo "the old absolute path: ", str_replace($base, "<base>", run("$second/", GeneratorOptions::encode("$first/worlds/w/stratum-pipeline.blob"), $seed, $logger)), "\n";
	echo "the new absolute path: ", run("$second/", GeneratorOptions::encode("$second/worlds/w/stratum-pipeline.blob"), $seed, $logger) === $terrain ? "the same terrain" : "different", "\n";
	echo "logged: ", count($logged), "\n";

	unlink("$second/worlds/w/stratum-pipeline.blob");
	rmdir("$second/worlds/w");
	rmdir("$second/worlds");
	rmdir($second);
	rmdir("$base/elsewhere");
	rmdir($base);
}
?>
--EXPECT--
bool(true)
stored options: {"blob":"worlds\/w\/stratum-pipeline.blob","settings":"minecraft:overworld","biomes":"minecraft:overworld"}
after the move: the same terrain
the old absolute path: refused at load: Stratum cannot read this world's frozen pipeline at <base>/server/worlds/w/stratum-pipeline.blob
the new absolute path: the same terrain
logged: 0
