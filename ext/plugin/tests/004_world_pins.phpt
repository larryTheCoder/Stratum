--TEST--
A Stratum world's state is pinned from world load to unload, so a worker restart neither drops held fluid updates nor rereads the blob
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
// PocketMine-MP's memory manager shuts idle generation workers down, and
// every generator on them — each holding the world's Stratum\Dimension —
// goes with them. Unpinned, the last one took the world's fluid-update
// outbox along, with the updates of every chunk generated but not yet
// populated; and the restarted worker read the blob from disk again,
// whatever it held by then. StratumWorlds pins the world on the main thread
// at WorldLoadEvent and unpins it at WorldUnloadEvent.
//
// Stand-ins as in 002_fluid_update_scheduler.phpt, read from PocketMine-MP
// 5.44.3's phar, plus WorldLoadEvent (a WorldEvent: getWorld()). A worker
// is what StratumGenerator's constructor does — GeneratorOptions::parse()
// of the options string, then Stratum\Dimension::open() — and its shutdown
// is that Dimension going. A chunk's population is ChunkPopulateEvent and
// then the server's ticks until its water is due (FluidUpdateScheduler
// ticks a liquid its tick rate after the population). The engine is the
// real extension.

namespace {
	interface Logger{
		public function error($message);
	}
}

namespace pocketmine {
	final class Server{
		private static ?self $instance = null;
		public int $tick = 0;
		public static function get() : self{ return self::$instance ??= new self(); }
		public function getTick() : int{ return $this->tick; }
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

namespace pocketmine\block {
	class Block{
		public function __construct(private \pocketmine\math\Vector3 $position){}
		final public function getPosition() : \pocketmine\math\Vector3{ return $this->position; }
	}
	abstract class Liquid extends Block{
		abstract public function tickRate() : int;
	}
	class Water extends Liquid{
		public function tickRate() : int{ return 5; }
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
			return match(strtolower($name)){
				"stratum" => new GeneratorManagerEntry("Stratum\\PMMP\\StratumGenerator"),
				"normal" => new GeneratorManagerEntry("pocketmine\\world\\generator\\normal\\Normal"),
				default => null
			};
		}
	}
}

namespace pocketmine\world {
	use pocketmine\math\Vector3;

	class World{
		/** @var list<array{int, int, int, int}> */
		public array $scheduled = [];

		public function __construct(
			private int $id,
			private string $generator,
			private string $generatorOptions,
			private int $seed
		){}

		public function getId() : int{ return $this->id; }
		public function getSeed() : int{ return $this->seed; }
		public function getFolderName() : string{ return "world" . $this->id; }
		public function getServer() : \pocketmine\Server{ return \pocketmine\Server::get(); }
		public function isLoaded() : bool{ return true; }
		public function isChunkLocked(int $chunkX, int $chunkZ) : bool{ return false; }
		public function getProvider() : object{
			$data = new class($this->generator, $this->generatorOptions){
				public function __construct(private string $generator, private string $options){}
				public function getGenerator() : string{ return $this->generator; }
				public function getGeneratorOptions() : string{ return $this->options; }
			};
			return new class($data){
				public function __construct(private object $data){}
				public function getWorldData() : object{ return $this->data; }
			};
		}
		public function getBlockAt(int $x, int $y, int $z, bool $cached = true, bool $addToCache = true) : \pocketmine\block\Block{
			return new \pocketmine\block\Water(new Vector3($x, $y, $z));
		}
		public function scheduleDelayedBlockUpdate(Vector3 $pos, int $delay) : void{
			$this->scheduled[] = [(int) $pos->x, (int) $pos->y, (int) $pos->z, $delay];
		}
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
	class ChunkPopulateEvent{
		public function __construct(private World $world, private int $chunkX, private int $chunkZ){}
		public function getWorld() : World{ return $this->world; }
		public function getChunkX() : int{ return $this->chunkX; }
		public function getChunkZ() : int{ return $this->chunkZ; }
	}
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";
	require __DIR__ . "/../src/StratumWorlds.php";
	require __DIR__ . "/../src/FluidUpdateScheduler.php";

	use pocketmine\event\world\ChunkPopulateEvent;
	use pocketmine\event\world\WorldLoadEvent;
	use pocketmine\event\world\WorldUnloadEvent;
	use pocketmine\world\World;
	use Stratum\PMMP\FluidUpdateScheduler;
	use Stratum\PMMP\GeneratorOptions;
	use Stratum\PMMP\StratumWorlds;

	$logged = [];
	$logger = new class($logged) implements Logger{
		public function __construct(private array &$logged){}
		public function error($message){ $this->logged[] = $message; }
	};

	/** What StratumGenerator's constructor does on a worker. */
	function worker(World $world, string $options) : Stratum\Dimension{
		$parsed = GeneratorOptions::parse($options);
		return Stratum\Dimension::open($parsed->blobPath, $parsed->noiseSettings, $parsed->biomeSource, $world->getSeed());
	}

	/** PocketMine-MP populates chunk ($x, $z) of $world, and ticks on until its water is due. */
	function populated(FluidUpdateScheduler $scheduler, World $world, int $x, int $z) : void{
		$scheduler->onChunkPopulate(new ChunkPopulateEvent($world, $x, $z));
		pocketmine\Server::get()->tick += 5;
		$scheduler->release();
	}

	/** The positions scheduled, without their delays. */
	function positions(World $world) : array{
		return array_map(fn($s) => [$s[0], $s[1], $s[2]], $world->scheduled);
	}

	$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
	$dir = sys_get_temp_dir() . "/stratum-pins-" . getmypid();
	@mkdir($dir);
	$seed = 20260915;
	$blobs = [];
	foreach(["pinned", "unpinned"] as $name){
		$blobs[$name] = "$dir/$name.blob";
		Stratum\freezePipeline($root, $blobs[$name]);
	}
	$garbage = "$dir/garbage.blob";
	file_put_contents($garbage, "not a pipeline at all");

	// What chunk (-3, 2) leaves, from a world of its own (its own blob
	// path, so its own outbox), taken at once.
	$probe = Stratum\Dimension::open($blobs["unpinned"], "minecraft:overworld", "minecraft:overworld", $seed);
	$encoded = $probe->encodeChunk(-3, 2);
	$expected = Stratum\takeFluidUpdates($blobs["unpinned"], "minecraft:overworld", "minecraft:overworld", $seed, -3, 2);
	unset($probe);
	echo "chunk -3 2 marks: ", count($expected) > 2 ? "several" : "too few", "\n";

	$worlds = new StratumWorlds($logger);
	$scheduler = new FluidUpdateScheduler($worlds);

	$options = GeneratorOptions::encode($blobs["pinned"]);
	$pinned = new World(1, "stratum", $options, $seed);
	$unpinnedOptions = GeneratorOptions::encode($blobs["unpinned"]);
	$unpinned = new World(2, "stratum", $unpinnedOptions, $seed);
	$normal = new World(3, "normal", $options, $seed);

	// World load. A world another generator owns is not pinned; the second
	// Stratum world is loaded without the listener, as the plugin did
	// before.
	$worlds->onWorldLoad(new WorldLoadEvent($pinned));
	$worlds->onWorldLoad(new WorldLoadEvent($normal));
	var_dump($worlds->isPinned($pinned), $worlds->isPinned($normal), $worlds->isPinned($unpinned));

	// Each world's worker generates chunk (-3, 2) as a neighbour, and is
	// shut down while idle before PocketMine-MP populates the chunk.
	foreach([[$pinned, $options], [$unpinned, $unpinnedOptions]] as [$world, $worldOptions]){
		$generator = worker($world, $worldOptions);
		$generator->encodeChunk(-3, 2);
		unset($generator);
	}
	populated($scheduler, $pinned, -3, 2);
	populated($scheduler, $unpinned, -3, 2);
	echo "pinned world, after its worker went: ", positions($pinned) === $expected ? "every update scheduled" : count($pinned->scheduled) . " scheduled", "\n";
	echo "unpinned world, after its worker went: ", count($unpinned->scheduled), " scheduled\n";

	// The pinned world's blob is replaced on disk while it is loaded. The
	// restarted worker generates what the world loaded with; the file is not
	// read.
	copy($garbage, $blobs["pinned"]);
	$restarted = worker($pinned, $options);
	var_dump($restarted->encodeChunk(-3, 2) === $encoded);
	unset($restarted);
	// And what it left is held through its going, as before.
	$pinned->scheduled = [];
	populated($scheduler, $pinned, -3, 2);
	echo "after the restarted worker went: ", positions($pinned) === $expected ? "every update scheduled" : count($pinned->scheduled) . " scheduled", "\n";

	// Another world's unload leaves the pin alone; its own unload releases
	// it, and a worker now reads the file — which refuses.
	$worlds->onWorldUnload(new WorldUnloadEvent($normal));
	var_dump($worlds->isPinned($pinned));
	$worlds->onWorldUnload(new WorldUnloadEvent($pinned));
	var_dump($worlds->isPinned($pinned), Stratum\unpinWorld($blobs["pinned"], "minecraft:overworld", "minecraft:overworld", $seed));
	try{
		worker($pinned, $options);
		echo "the unpinned worker opened\n";
	}catch(Stratum\GenerationException $e){
		echo "unpinned, a worker reads the replaced file: ", $e->getMessage(), "\n";
	}

	// A world whose blob cannot be opened when it loads — replaced after
	// GeneratorOptions::validate() passed it — is logged by name, not
	// thrown out of the event, and not pinned.
	$late = new World(4, "stratum", GeneratorOptions::encode($garbage), $seed);
	$worlds->onWorldLoad(new WorldLoadEvent($late));
	var_dump($worlds->isPinned($late));
	echo "logged: ", count($logged), "\n", str_replace($garbage, "<garbage>", $logged[0] ?? ""), "\n";

	// The plugin's onDisable(): every pin this made goes.
	Stratum\freezePipeline($root, $blobs["pinned"]);
	$reloaded = new World(5, "stratum", $options, $seed);
	$worlds->onWorldLoad(new WorldLoadEvent($reloaded));
	var_dump($worlds->isPinned($reloaded));
	$worlds->unpinAll();
	var_dump($worlds->isPinned($reloaded), Stratum\unpinWorld($blobs["pinned"], "minecraft:overworld", "minecraft:overworld", $seed));

	foreach([...array_values($blobs), $garbage] as $file){
		unlink($file);
	}
	rmdir($dir);
}
?>
--EXPECTF--
chunk -3 2 marks: several
bool(true)
bool(false)
bool(false)
pinned world, after its worker went: every update scheduled
unpinned world, after its worker went: 0 scheduled
bool(true)
after the restarted worker went: every update scheduled
bool(true)
bool(false)
bool(false)
unpinned, a worker reads the replaced file: not a frozen pipeline: %s
bool(false)
logged: 1
Stratum world "world4": its state could not be pinned on the main thread, so its generators will open its frozen pipeline themselves: not a frozen pipeline: %s
bool(true)
bool(false)
bool(false)
