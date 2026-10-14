--TEST--
The plugin ticks each liquid the engine marked, once, at its tick rate, in Stratum worlds only, only once no population round its chunk is in flight or still to come, and looks at a held chunk only when something round it changes
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
// PocketMine-MP is not loaded here (ext/plugin/README.md), so the classes
// FluidUpdateScheduler and StratumWorlds (which says which worlds are
// Stratum's) touch are stand-ins with the signatures they call, read from
// PocketMine-MP 5.44.3's phar: World::getBlockAt(int, int, int),
// World::scheduleDelayedBlockUpdate(Vector3, int), World::isChunkLocked(int,
// int), World::getChunk(int, int) (a loaded chunk or null, never read from
// disk) and Chunk::isPopulated(), World::isLoaded(),
// World::getServer()->getTick(), World::getId/
// getSeed/getFolderName/getProvider()->getWorldData()->getGenerator/
// getGeneratorOptions, Block::getPosition(), Liquid::tickRate() (water 5,
// lava 30), ChunkEvent::getChunkX/getChunkZ/getChunk, WorldEvent::getWorld,
// ChunkLoadEvent::isNewChunk,
// GeneratorManager::getInstance()->getGenerator(name)?->getGeneratorClass().
// What PocketMine-MP does round them, read from the same phar: a population
// task locks the 3x3 chunks it populates (World::lockChunk) until its
// result is back, and only round a chunk not yet populated; its result
// fires ChunkLoadEvent for each chunk it adds and then ChunkPopulateEvent
// for the centre (World::generateChunkCallback); a chunk read from disk
// fires ChunkLoadEvent (World::loadChunk), and one unloaded ChunkUnloadEvent
// (World::unloadChunk). The stand-in World counts every chunk lookup the
// scheduler makes, getChunk() and isChunkLocked() alike. The engine side is
// the real extension: the updates are the ones Dimension::encodeChunk
// left, taken through Stratum\takeFluidUpdates.

namespace {
	interface Logger{
		public function error($message);
	}
}

namespace pocketmine {
	final class Server{
		public int $tick = 0;
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
	class Lava extends Liquid{
		public function tickRate() : int{ return 30; }
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

namespace pocketmine\world\format {
	final class Chunk{
		public function __construct(private bool $populated){}
		public function isPopulated() : bool{ return $this->populated; }
	}
}

namespace pocketmine\world {
	use pocketmine\math\Vector3;
	use pocketmine\Server;
	use pocketmine\world\format\Chunk;

	class World{
		/** @var list<array{int, int, int, int, int}> x, y, z, delay, the tick it was scheduled on */
		public array $scheduled = [];
		/** @var array<string, true> "x z" of every locked chunk */
		public array $locked = [];
		/** @var array<string, bool> "x z" of every loaded chunk => whether it is populated */
		public array $chunks = [];
		public bool $loaded = true;
		/** Every getChunk() and isChunkLocked() call made of this world. */
		public int $lookups = 0;

		/** @param \Closure(int, int, int) : \pocketmine\block\Block $blockAt */
		public function __construct(
			private int $id,
			private string $generator,
			private string $generatorOptions,
			private int $seed,
			private \Closure $blockAt,
			private Server $server
		){}

		public function getId() : int{ return $this->id; }
		public function getSeed() : int{ return $this->seed; }
		public function getServer() : Server{ return $this->server; }
		public function isLoaded() : bool{ return $this->loaded; }
		public function getFolderName() : string{ return "world" . $this->id; }
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
			return ($this->blockAt)($x, $y, $z);
		}
		public function scheduleDelayedBlockUpdate(Vector3 $pos, int $delay) : void{
			$this->scheduled[] = [(int) $pos->x, (int) $pos->y, (int) $pos->z, $delay, $this->server->getTick()];
		}
		public function isChunkLocked(int $chunkX, int $chunkZ) : bool{
			++$this->lookups;
			return isset($this->locked["$chunkX $chunkZ"]);
		}
		/** A loaded chunk, never one read from disk. */
		public function getChunk(int $chunkX, int $chunkZ) : ?Chunk{
			++$this->lookups;
			return isset($this->chunks["$chunkX $chunkZ"]) ? new Chunk($this->chunks["$chunkX $chunkZ"]) : null;
		}
		/** Every chunk within $radius of ($chunkX, $chunkZ), loaded and populated. */
		public function populateAround(int $chunkX, int $chunkZ, int $radius) : void{
			for($x = $chunkX - $radius; $x <= $chunkX + $radius; ++$x){
				for($z = $chunkZ - $radius; $z <= $chunkZ + $radius; ++$z){
					$this->chunks["$x $z"] = true;
				}
			}
		}
	}
}

namespace pocketmine\event\world {
	use pocketmine\world\format\Chunk;
	use pocketmine\world\World;

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
	class ChunkLoadEvent{
		public function __construct(private World $world, private int $chunkX, private int $chunkZ, private Chunk $chunk, private bool $newChunk){}
		public function getWorld() : World{ return $this->world; }
		public function getChunkX() : int{ return $this->chunkX; }
		public function getChunkZ() : int{ return $this->chunkZ; }
		public function getChunk() : Chunk{ return $this->chunk; }
		public function isNewChunk() : bool{ return $this->newChunk; }
	}
	class ChunkUnloadEvent{
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

	use pocketmine\block\Block;
	use pocketmine\block\Lava;
	use pocketmine\block\Water;
	use pocketmine\event\world\ChunkLoadEvent;
	use pocketmine\event\world\ChunkPopulateEvent;
	use pocketmine\event\world\ChunkUnloadEvent;
	use pocketmine\event\world\WorldUnloadEvent;
	use pocketmine\math\Vector3;
	use pocketmine\Server;
	use pocketmine\world\format\Chunk;
	use pocketmine\world\World;
	use Stratum\PMMP\FluidUpdateScheduler;
	use Stratum\PMMP\StratumWorlds;
	use Stratum\PMMP\GeneratorOptions;

	$logged = [];
	$logger = new class($logged) implements Logger{
		public function __construct(private array &$logged){}
		public function error($message){ $this->logged[] = $message; }
	};

	$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
	$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
	Stratum\freezePipeline($root, $blob);
	$seed = 20260915;
	$options = GeneratorOptions::encode($blob);

	// The worker's side: the world's generator encodes chunk (-3, 2), which
	// holds aquifer fluid (ext/zend/tests/004_fluid_updates.phpt).
	$dimension = Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:overworld", $seed);
	$dimension->encodeChunk(-3, 2);
	$expected = Stratum\takeFluidUpdates($blob, "minecraft:overworld", "minecraft:overworld", $seed, -3, 2);
	echo "marked: ", count($expected) > 2 ? "several" : "too few", "\n";
	$dimension->encodeChunk(-3, 2); // left again, for the scheduler to take

	// The first marked position now holds stone and the second lava; the rest
	// water.
	$blockAt = function(int $x, int $y, int $z) use ($expected) : Block{
		$at = new Vector3($x, $y, $z);
		return match([$x, $y, $z]){
			$expected[0] => new Block($at),
			$expected[1] => new Lava($at),
			default => new Water($at)
		};
	};
	$positions = fn(array $scheduled) => array_map(fn($s) => [$s[0], $s[1], $s[2]], $scheduled);
	/** Runs release() on each server tick until $until, and returns how many chunk lookups it made. */
	$tickUntil = function(World $world, int $until) use (&$server, &$scheduler) : int{
		$before = $world->lookups;
		for(; $server->tick < $until; ++$server->tick){
			$scheduler->release();
		}
		return $world->lookups - $before;
	};

	$server = new Server();
	$server->tick = 100;
	$worlds = new StratumWorlds($logger, static function(\Closure $task) : void{});
	$scheduler = new FluidUpdateScheduler($worlds);

	// A world another generator owns holds nothing and takes nothing, even
	// with the same options and seed, populated or loaded.
	$normal = new World(1, "normal", $options, $seed, $blockAt, $server);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($normal, -3, 2));
	$scheduler->onChunkLoad(new ChunkLoadEvent($normal, -3, 2, new Chunk(true), false));
	echo "normal world held: ", $scheduler->chunksHeld(), "\n";

	// The Stratum world holds the chunk as it is populated, on tick 100, and
	// ticks nothing while a chunk two away, (-1, 4), is not populated: its
	// population would lock (-2, 3), in the 3x3 round this chunk, and a flow
	// there would make PocketMine-MP discard its result. The first release()
	// finds that out — 25 lookups, (-1, 4) the last of the 5x5 — and none
	// after looks again, whether (-1, 4) stays loaded unpopulated or is
	// unloaded: nothing has changed that could settle the chunk.
	$stratum = new World(2, "stratum", $options, $seed, $blockAt, $server);
	$stratum->populateAround(-3, 2, 2);
	$stratum->chunks["-1 4"] = false;
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	echo "held: ", $scheduler->chunksHeld(), " chunk, ", $scheduler->chunksDue(), " due, ", count($stratum->scheduled), " scheduled\n";
	$looked = $tickUntil($stratum, 105);
	echo "while (-1, 4) is unpopulated: ", count($stratum->scheduled), " scheduled, ", $looked, " lookups over 5 ticks, ", $scheduler->chunksDue(), " due\n";
	unset($stratum->chunks["-1 4"]);
	$looked = $tickUntil($stratum, 107);
	echo "while (-1, 4) is not loaded: ", count($stratum->scheduled), " scheduled, ", $looked, " lookups over 2 ticks\n";

	// Tick 107: (-1, 4) is populated, and its ChunkPopulateEvent has the
	// chunk looked at again — and (-1, 4) held in its turn. Every chunk
	// within two is now populated, but another plugin holds (-2, 3), in the
	// 3x3, locked: nothing is ticked while it does, and the chunk is looked
	// at every tick, since no event says when a lock goes. A lock outside
	// the 3x3, at (-1, 2), holds nothing back. (-1, 4) itself waits on the
	// chunks beyond it, looked at once.
	$stratum->chunks["-1 4"] = true;
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -1, 4));
	$stratum->locked["-2 3"] = true;
	$stratum->locked["-1 2"] = true;
	echo "after (-1, 4)'s population: ", $scheduler->chunksHeld(), " held, ", $scheduler->chunksDue(), " due\n";
	$tickUntil($stratum, 110);
	echo "while (-2, 3) is locked: ", count($stratum->scheduled), " scheduled, ", $scheduler->chunksDue(), " due\n";

	// Tick 110: the lock is gone, and the chunk has settled for good. Its
	// liquids are taken from the extension and scheduled at once, in the
	// engine's order, each its own tick rate after the population: the water
	// (due on tick 105) with delay 0, so PocketMine-MP ticks it this server
	// tick, after the plugin's scheduler; the lava (due on 130) with delay
	// 20. The stone is left alone.
	unset($stratum->locked["-2 3"]);
	$scheduler->release();
	var_dump($positions($stratum->scheduled) === array_slice($expected, 1));
	echo "on tick ", implode(",", array_unique(array_column($stratum->scheduled, 4))), ": lava delay ", $stratum->scheduled[0][3], ", water delay ", implode(",", array_unique(array_column(array_slice($stratum->scheduled, 1), 3))), "\n";
	echo "held after: ", $scheduler->chunksHeld(), ", ", $scheduler->chunksDue(), " due\n";

	// Once: populating it again ticks nothing more, and a chunk nothing was
	// left for is held until it settles, then let go with nothing ticked.
	// (-1, 4) still waits on the chunks beyond it.
	$stratum->scheduled = [];
	$stratum->populateAround(0, 0, 2);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, 0, 0));
	echo "repeats held: ", $scheduler->chunksHeld(), "\n";
	$server->tick += 40;
	$scheduler->release();
	echo "after repeats: ", count($stratum->scheduled), " scheduled, ", $scheduler->chunksHeld(), " held, ", $scheduler->chunksDue(), " due\n";

	// Unloaded before it settles, a held chunk is let go; its liquids stay in
	// the extension. Loaded again, populated, it is held again from that
	// tick, and released once it settles: tick 200, the water delay 5 and
	// the lava 30. A chunk loaded unpopulated holds nothing.
	$dimension->encodeChunk(-3, 2);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	unset($stratum->chunks["-5 0"]);
	$scheduler->release();
	echo "held, waiting on (-5, 0): ", $scheduler->chunksHeld(), " held, ", $scheduler->chunksDue(), " due\n";
	unset($stratum->chunks["-3 2"]);
	$scheduler->onChunkUnload(new ChunkUnloadEvent($stratum, -3, 2));
	echo "after its unload: ", $scheduler->chunksHeld(), " held\n";
	$server->tick = 200;
	$stratum->chunks["-3 2"] = true;
	$stratum->chunks["-5 0"] = true;
	$scheduler->onChunkLoad(new ChunkLoadEvent($stratum, -3, 2, new Chunk(true), false));
	$scheduler->onChunkLoad(new ChunkLoadEvent($stratum, -9, 9, new Chunk(false), true));
	echo "loaded again: ", $scheduler->chunksHeld(), " held, ", $scheduler->chunksDue(), " due\n";
	$scheduler->release();
	var_dump($positions($stratum->scheduled) === array_slice($expected, 1));
	echo "on tick ", implode(",", array_unique(array_column($stratum->scheduled, 4))), ": lava delay ", $stratum->scheduled[0][3], ", water delay ", implode(",", array_unique(array_column(array_slice($stratum->scheduled, 1), 3))), "\n";

	// The edge of what is populated, at scale: 2000 chunks in a row, each
	// held, none with every chunk within two of it populated. The first
	// release() looks at each once — one lookup each, the first chunk of its
	// 5x5 missing — and from then on not one, however many ticks pass.
	$edge = new World(5, "stratum", $options, $seed, $blockAt, $server);
	for($x = 0; $x < 2000; ++$x){
		$edge->chunks["$x 100"] = true;
		$scheduler->onChunkPopulate(new ChunkPopulateEvent($edge, $x, 100));
	}
	$looked = $tickUntil($edge, $server->tick + 1);
	echo "edge: ", $scheduler->chunksHeld() - 1, " held, ", $looked, " lookups on the first tick, ";
	echo $tickUntil($edge, $server->tick + 100), " over the next 100\n";
	// Unloaded, they are let go.
	for($x = 0; $x < 2000; ++$x){
		unset($edge->chunks["$x 100"]);
		$scheduler->onChunkUnload(new ChunkUnloadEvent($edge, $x, 100));
	}
	echo "edge unloaded: ", $scheduler->chunksHeld() - 1, " held\n";

	// A world whose options do not parse — which PocketMine-MP, having
	// validated them at load, never presents — says so once, by name,
	// rather than throwing out of the tick.
	$broken = new World(3, "stratum", '{"blob":"/nonexistent/stratum.blob","biome":"minecraft:the_end"}', $seed, $blockAt, $server);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($broken, -3, 2));
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($broken, -3, 2));
	echo "logged: ", count($logged), "\n";
	echo str_replace($blob, "<blob>", $logged[0]), "\n";
	// A blob that has gone is no longer the options' concern (parse() reads
	// the string alone): such a world holds its chunk until it settles, then
	// finds nothing in the extension and ticks nothing, without a word.
	$gone = new World(4, "stratum", GeneratorOptions::encode("/nonexistent/stratum.blob"), $seed, $blockAt, $server);
	$gone->populateAround(-3, 2, 2);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($gone, -3, 2));
	$scheduler->release();
	echo "gone: ", count($gone->scheduled), " scheduled, ", $scheduler->chunksHeld() - 1, " held, ", count($logged), " logged\n";

	// A held chunk goes with its world: unloaded before it settles, its
	// liquids are never ticked.
	$stratum->scheduled = [];
	$dimension->encodeChunk(-3, 2);
	$stratum->locked["-2 3"] = true;
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	echo "held again: ", $scheduler->chunksHeld(), "\n";
	$worlds->onWorldUnload(new WorldUnloadEvent($stratum));
	$scheduler->onWorldUnload(new WorldUnloadEvent($stratum));
	$stratum->loaded = false;
	$server->tick += 40;
	$scheduler->release();
	echo "after unload: ", count($stratum->scheduled), " scheduled, ", $scheduler->chunksHeld(), " held, ", $scheduler->chunksDue(), " due\n";

	// What a world's options say is read once, and forgotten at unload: the
	// same id, after an unload, is read afresh.
	$dimension->encodeChunk(-3, 2);
	$reused = new World(2, "normal", $options, $seed, $blockAt, $server);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($reused, -3, 2));
	$scheduler->onChunkLoad(new ChunkLoadEvent($reused, -3, 2, new Chunk(true), false));
	echo "after unload, as another generator's: ", $scheduler->chunksHeld(), " held\n";
	echo "still in the extension for the Stratum world: ", count(Stratum\takeFluidUpdates($blob, "minecraft:overworld", "minecraft:overworld", $seed, -3, 2)) === count($expected) ? "yes" : "no", "\n";

	unset($dimension);
	unlink($blob);
}
?>
--EXPECT--
marked: several
normal world held: 0
held: 1 chunk, 1 due, 0 scheduled
while (-1, 4) is unpopulated: 0 scheduled, 25 lookups over 5 ticks, 0 due
while (-1, 4) is not loaded: 0 scheduled, 0 lookups over 2 ticks
after (-1, 4)'s population: 2 held, 2 due
while (-2, 3) is locked: 0 scheduled, 1 due
bool(true)
on tick 110: lava delay 20, water delay 0
held after: 1, 0 due
repeats held: 3
after repeats: 0 scheduled, 1 held, 0 due
held, waiting on (-5, 0): 2 held, 0 due
after its unload: 1 held
loaded again: 2 held, 2 due
bool(true)
on tick 200: lava delay 30, water delay 5
edge: 2000 held, 2000 lookups on the first tick, 0 over the next 100
edge unloaded: 0 held
logged: 1
Stratum world "world3": its aquifer water and lava will not flow, because its generator options do not parse: Stratum's generator options take "blob", "settings" and "biomes" and nothing else, so "biome" would be ignored: {"blob":"/nonexistent/stratum.blob","biome":"minecraft:the_end"}
gone: 0 scheduled, 0 held, 1 logged
held again: 2
after unload: 0 scheduled, 0 held, 0 due
after unload, as another generator's: 0 held
still in the extension for the Stratum world: yes
