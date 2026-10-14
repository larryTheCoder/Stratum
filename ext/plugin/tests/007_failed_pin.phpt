--TEST--
A world whose pin fails at load reaches no generation worker: its spawn area is locked against population, and it is unloaded on the next tick, or the server stopped if it is the default world
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
// The pin StratumWorlds makes at WorldLoadEvent is the same full open a
// generator makes, so a world whose pin fails would fail on its first
// generation worker, and an exception there takes PocketMine-MP down. It
// used to be logged and let through.
//
// Stand-ins for what StratumWorlds touches, and for what PocketMine-MP
// 5.44.3 does around it, read from its phar:
//  - WorldManager::generateWorld() fires WorldLoadEvent and then, on the
//    same World, orders the population of the chunks round its spawn
//    (ChunkSelector::selectChunks(8, ...): at most 8 away on each axis);
//  - World::internalOrderChunkPopulation() queues a population whose 3x3
//    holds a locked chunk instead of handing it to a worker, and
//    World::lockChunk() throws on a chunk already locked;
//  - WorldManager::unloadWorld($world, true) fires WorldUnloadEvent and
//    unloads the world, whose unload rejects every queued population;
//  - the plugin's task scheduler runs a task on the next server tick
//    (here, by hand); Server::shutdown() stops the server.
// The extension is real.

namespace {
	interface Logger{
		public function error($message);
		public function emergency($message);
	}
}

namespace pocketmine\math {
	class Vector3{
		public function __construct(public int|float $x, public int|float $y, public int|float $z){}
		public function getFloorX() : int{ return (int) floor($this->x); }
		public function getFloorZ() : int{ return (int) floor($this->z); }
	}
}

namespace pocketmine\event {
	interface Listener{}
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

namespace pocketmine {
	final class Server{
		public int $shutdowns = 0;
		public function __construct(private world\WorldManager $worldManager){}
		public function getWorldManager() : world\WorldManager{ return $this->worldManager; }
		public function shutdown() : void{ ++$this->shutdowns; }
	}
}

namespace pocketmine\world {
	use pocketmine\event\world\WorldUnloadEvent;
	use pocketmine\math\Vector3;
	use pocketmine\Server;
	use Stratum\PMMP\StratumWorlds;

	final class ChunkLockId{}

	final class WorldManager{
		public ?World $default = null;
		/** @var list<array{string, bool}> folder name, force */
		public array $unloaded = [];
		public ?StratumWorlds $listener = null;
		public function getDefaultWorld() : ?World{ return $this->default; }
		public function unloadWorld(World $world, bool $forceUnload = false) : bool{
			$this->listener?->onWorldUnload(new WorldUnloadEvent($world));
			$this->unloaded[] = [$world->getFolderName(), $forceUnload];
			$world->onUnload();
			return true;
		}
	}

	class World{
		/** @var array<string, ChunkLockId> "x z" => the lock */
		public array $locks = [];
		/** @var list<string> "x z" of each population queued, not yet run */
		public array $queued = [];
		/** @var list<string> "x z" of each population handed to a worker */
		public array $toWorkers = [];
		public int $rejected = 0;
		private bool $loaded = true;

		public function __construct(
			private int $id,
			private string $name,
			private string $generatorOptions,
			private int $seed,
			private Server $server,
			private Vector3 $spawn = new Vector3(256, 70, 256)
		){}

		public function getId() : int{ return $this->id; }
		public function getSeed() : int{ return $this->seed; }
		public function getFolderName() : string{ return $this->name; }
		public function getServer() : Server{ return $this->server; }
		public function isLoaded() : bool{ return $this->loaded; }
		public function getSpawnLocation() : Vector3{ return $this->spawn; }
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
		public function isChunkLocked(int $chunkX, int $chunkZ) : bool{
			return isset($this->locks["$chunkX $chunkZ"]);
		}
		public function lockChunk(int $chunkX, int $chunkZ, ChunkLockId $lockId) : void{
			if(isset($this->locks["$chunkX $chunkZ"])){
				throw new \InvalidArgumentException("Chunk $chunkX $chunkZ is already locked");
			}
			$this->locks["$chunkX $chunkZ"] = $lockId;
		}
		/** World::orderChunkPopulation's lock check, as internalOrderChunkPopulation makes it. */
		public function orderChunkPopulation(int $chunkX, int $chunkZ) : void{
			for($x = -1; $x <= 1; ++$x){
				for($z = -1; $z <= 1; ++$z){
					if($this->isChunkLocked($chunkX + $x, $chunkZ + $z)){
						$this->queued[] = "$chunkX $chunkZ";
						return;
					}
				}
			}
			$this->toWorkers[] = "$chunkX $chunkZ";
		}
		public function onUnload() : void{
			$this->rejected += count($this->queued);
			$this->queued = [];
			$this->loaded = false;
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
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";
	require __DIR__ . "/../src/StratumWorlds.php";

	use pocketmine\event\world\WorldLoadEvent;
	use pocketmine\math\Vector3;
	use pocketmine\Server;
	use pocketmine\world\ChunkLockId;
	use pocketmine\world\World;
	use pocketmine\world\WorldManager;
	use Stratum\PMMP\GeneratorOptions;
	use Stratum\PMMP\StratumWorlds;

	$logged = [];
	$logger = new class($logged) implements Logger{
		public function __construct(private array &$logged){}
		public function error($message){ $this->logged[] = "error: " . $message; }
		public function emergency($message){ $this->logged[] = "emergency: " . $message; }
	};
	/** @var list<\Closure() : void> $nextTick */
	$nextTick = [];
	$worlds = new StratumWorlds($logger, static function(\Closure $task) use (&$nextTick) : void{
		$nextTick[] = $task;
	});
	$tick = static function() use (&$nextTick) : void{
		$tasks = $nextTick;
		$nextTick = [];
		foreach($tasks as $task){
			$task();
		}
	};

	$manager = new WorldManager();
	$manager->listener = $worlds;
	$server = new Server($manager);
	$seed = 20260915;
	$garbage = tempnam(sys_get_temp_dir(), "stratum-blob-");
	file_put_contents($garbage, "not a pipeline at all");

	/**
	 * What generateWorld() does after WorldLoadEvent: order the spawn area.
	 * Every chunk selectChunks(8, ...) can yield, -8 to 7 from the spawn
	 * chunk on each axis, and the corners it leaves out.
	 */
	function orderSpawnArea(World $world) : void{
		$spawnX = $world->getSpawnLocation()->getFloorX() >> 4;
		$spawnZ = $world->getSpawnLocation()->getFloorZ() >> 4;
		for($x = -8; $x <= 7; ++$x){
			for($z = -8; $z <= 7; ++$z){
				$world->orderChunkPopulation($spawnX + $x, $spawnZ + $z);
			}
		}
	}

	// A new world whose blob was replaced after validate() passed it: the
	// pin fails, by name, and nothing is thrown out of the event.
	$late = new World(4, "late", GeneratorOptions::encode($garbage), $seed, $server);
	// A population already in flight at (16, 16), whose lock is left alone.
	$inFlight = new ChunkLockId();
	$late->lockChunk(16, 16, $inFlight);
	$worlds->onWorldLoad(new WorldLoadEvent($late));
	var_dump($worlds->isPinned($late));
	echo str_replace($garbage, "<garbage>", $logged[0]), "\n";

	// The spawn area, (256, 70, 256)'s chunk and the 16 chunks round it on
	// each axis, is locked under one lock of StratumWorlds' own.
	$locks = array_diff_key($late->locks, ["16 16" => true]);
	echo "locked: ", count($locks), " chunks under ", count(array_unique(array_map("spl_object_id", $locks))), " lock, from ",
		min(array_map(fn($k) => (int) explode(" ", $k)[0], array_keys($late->locks))), " to ",
		max(array_map(fn($k) => (int) explode(" ", $k)[0], array_keys($late->locks))), "\n";
	var_dump($late->locks["16 16"] === $inFlight);

	// generateWorld() then orders the spawn area: every population is
	// queued, and none reaches a worker. One ordered outside that area
	// before the next tick would: that is the window left open.
	orderSpawnArea($late);
	echo "spawn area: ", count($late->queued), " queued, ", count($late->toWorkers), " to a worker\n";
	$late->orderChunkPopulation(100, 100);
	echo "outside it: ", count($late->toWorkers), " to a worker\n";
	var_dump($late->isLoaded());

	// The next tick: force-unloaded, which rejects every queued population.
	$tick();
	echo "unloaded: ", json_encode($manager->unloaded), ", ", $late->rejected, " populations rejected, loaded: ", var_export($late->isLoaded(), true), "\n";
	echo "server stopped: ", $server->shutdowns, "\n";

	// The default world cannot be unloaded and leave a server players can
	// join: it is not unloaded, and the server is stopped, by name.
	$default = new World(5, "default", GeneratorOptions::encode($garbage), $seed, $server, new Vector3(-40, 64, 300));
	$manager->default = $default;
	$worlds->onWorldLoad(new WorldLoadEvent($default));
	echo "default locked round its spawn: ", isset($default->locks["-3 18"], $default->locks["-11 10"], $default->locks["5 26"]) && !isset($default->locks["6 18"]) ? "yes" : "no", "\n";
	$tick();
	echo "server stopped: ", $server->shutdowns, ", unloaded: ", count($manager->unloaded), "\n";
	echo str_replace($garbage, "<garbage>", $logged[2]), "\n";

	// A world unloaded by something else before the next tick is left alone.
	$gone = new World(6, "gone", GeneratorOptions::encode($garbage), $seed, $server);
	$worlds->onWorldLoad(new WorldLoadEvent($gone));
	$manager->unloadWorld($gone);
	$tick();
	echo "unloaded once: ", count(array_filter($manager->unloaded, fn($u) => $u[0] === "gone")), "\n";

	// A world whose pin holds is pinned, and nothing else is done to it.
	$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
	Stratum\freezePipeline(getenv("STRATUM_FIXTURES_DIR") . "/1.21.11", $blob);
	$fine = new World(7, "fine", GeneratorOptions::encode($blob), $seed, $server);
	$worlds->onWorldLoad(new WorldLoadEvent($fine));
	orderSpawnArea($fine);
	echo "fine: pinned ", var_export($worlds->isPinned($fine), true), ", ", count($fine->locks), " locked, ", count($fine->toWorkers), " to workers, ", count($nextTick), " deferred\n";
	echo "logged: ", count($logged), "\n";

	$worlds->unpinAll();
	unlink($blob);
	unlink($garbage);
}
?>
--EXPECTF--
bool(false)
error: Stratum world "late" cannot generate: its state could not be pinned on the main thread (not a frozen pipeline: %s). No chunk of it is handed to a generation worker, where the same refusal would take the server down, and it is unloaded on the next tick
locked: 288 chunks under 1 lock, from 8 to 24
bool(true)
spawn area: 256 queued, 0 to a worker
outside it: 1 to a worker
bool(true)
unloaded: [["late",true]], 256 populations rejected, loaded: false
server stopped: 0
default locked round its spawn: yes
server stopped: 1, unloaded: 1
emergency: Stratum world "default" is the default world and cannot generate (its state could not be pinned); stopping the server rather than run without one
unloaded once: 1
fine: pinned true, 0 locked, 256 to workers, 0 deferred
logged: 4
