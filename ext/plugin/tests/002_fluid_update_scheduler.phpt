--TEST--
The plugin ticks each liquid the engine marked, once, at its tick rate, in Stratum worlds only, and only once no population round its chunk is in flight or still to come
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
// lava 30), ChunkEvent::getChunkX/getChunkZ, WorldEvent::getWorld,
// GeneratorManager::getInstance()->getGenerator(name)?->getGeneratorClass().
// A population task in flight is what World::internalOrderChunkPopulation
// leaves: the 3x3 chunks it populates locked (World::lockChunk) until its
// result is back. The engine side is the real extension: the updates are
// the ones Dimension::encodeChunk left, taken through
// Stratum\takeFluidUpdates.

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
			return isset($this->locked["$chunkX $chunkZ"]);
		}
		/** A loaded chunk, never one read from disk. */
		public function getChunk(int $chunkX, int $chunkZ) : ?Chunk{
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
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";
	require __DIR__ . "/../src/StratumWorlds.php";
	require __DIR__ . "/../src/FluidUpdateScheduler.php";

	use pocketmine\block\Block;
	use pocketmine\block\Lava;
	use pocketmine\block\Water;
	use pocketmine\event\world\ChunkPopulateEvent;
	use pocketmine\event\world\WorldUnloadEvent;
	use pocketmine\math\Vector3;
	use pocketmine\Server;
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
	$waters = array_slice($expected, 2);

	$server = new Server();
	$server->tick = 100;
	$worlds = new StratumWorlds($logger, static function(\Closure $task) : void{});
	$scheduler = new FluidUpdateScheduler($worlds);

	// A world another generator owns holds nothing and takes nothing, even
	// with the same options and seed.
	$normal = new World(1, "normal", $options, $seed, $blockAt, $server);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($normal, -3, 2));
	echo "normal world held: ", $scheduler->chunksHeld(), "\n";

	// The Stratum world holds the chunk as it is populated, on tick 100, and
	// ticks nothing while a chunk two away, (-1, 4), is not populated: its
	// population would lock (-2, 3), in the 3x3 round this chunk, and a flow
	// there would make PocketMine-MP discard its result. Loaded but
	// unpopulated, or not loaded at all, is the same.
	$stratum = new World(2, "stratum", $options, $seed, $blockAt, $server);
	$stratum->populateAround(-3, 2, 2);
	$stratum->chunks["-1 4"] = false;
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	echo "held: ", $scheduler->chunksHeld(), " chunk, ", count($stratum->scheduled), " scheduled\n";
	for(; $server->tick < 105; ++$server->tick){
		$scheduler->release();
	}
	unset($stratum->chunks["-1 4"]);
	for(; $server->tick < 107; ++$server->tick){
		$scheduler->release();
	}
	echo "while (-1, 4) is unpopulated: ", count($stratum->scheduled), " scheduled\n";

	// Tick 107: (-1, 4) is populated, but a population is in flight next to
	// the chunk, holding (-2, 3) of its 3x3: nothing is ticked while it runs
	// either. A lock outside the 3x3, at (-1, 2), holds nothing back.
	$stratum->chunks["-1 4"] = true;
	$stratum->locked["-2 3"] = true;
	$stratum->locked["-1 2"] = true;
	for(; $server->tick < 110; ++$server->tick){
		$scheduler->release();
	}
	echo "while (-2, 3) is locked: ", count($stratum->scheduled), " scheduled\n";

	// Tick 110: its result is back, and the chunk has settled for good. Its
	// liquids are taken from the extension and scheduled at once, in the
	// engine's order, each its own tick rate after the population: the water
	// (due on tick 105) with delay 0, so PocketMine-MP ticks it this server
	// tick, after the plugin's scheduler; the lava (due on 130) with delay
	// 20. The stone is left alone.
	unset($stratum->locked["-2 3"]);
	$scheduler->release();
	var_dump($positions($stratum->scheduled) === array_slice($expected, 1));
	echo "on tick ", implode(",", array_unique(array_column($stratum->scheduled, 4))), ": lava delay ", $stratum->scheduled[0][3], ", water delay ", implode(",", array_unique(array_column(array_slice($stratum->scheduled, 1), 3))), "\n";
	echo "held after: ", $scheduler->chunksHeld(), "\n";

	// Once: populating it again ticks nothing more, and a chunk nothing was
	// left for is held until it settles, then let go with nothing ticked.
	$stratum->scheduled = [];
	$stratum->populateAround(0, 0, 2);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, 0, 0));
	echo "repeats held: ", $scheduler->chunksHeld(), "\n";
	$server->tick += 40;
	$scheduler->release();
	echo "after repeats: ", count($stratum->scheduled), " scheduled, ", $scheduler->chunksHeld(), " held\n";

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
	echo "gone: ", count($gone->scheduled), " scheduled, ", $scheduler->chunksHeld(), " held, ", count($logged), " logged\n";

	// A held chunk goes with its world: unloaded before it settles, its
	// liquids are never ticked.
	$dimension->encodeChunk(-3, 2);
	$stratum->locked["-2 3"] = true;
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	echo "held again: ", $scheduler->chunksHeld(), "\n";
	$worlds->onWorldUnload(new WorldUnloadEvent($stratum));
	$scheduler->onWorldUnload(new WorldUnloadEvent($stratum));
	$stratum->loaded = false;
	$server->tick += 40;
	$scheduler->release();
	echo "after unload: ", count($stratum->scheduled), " scheduled, ", $scheduler->chunksHeld(), " held\n";

	// What a world's options say is read once, and forgotten at unload: the
	// same id, after an unload, is read afresh.
	$dimension->encodeChunk(-3, 2);
	$reused = new World(2, "normal", $options, $seed, $blockAt, $server);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($reused, -3, 2));
	echo "after unload, as another generator's: ", $scheduler->chunksHeld(), " held\n";
	echo "still in the extension for the Stratum world: ", count(Stratum\takeFluidUpdates($blob, "minecraft:overworld", "minecraft:overworld", $seed, -3, 2)) === count($expected) ? "yes" : "no", "\n";

	unset($dimension);
	unlink($blob);
}
?>
--EXPECT--
marked: several
normal world held: 0
held: 1 chunk, 0 scheduled
while (-1, 4) is unpopulated: 0 scheduled
while (-2, 3) is locked: 0 scheduled
bool(true)
on tick 110: lava delay 20, water delay 0
held after: 0
repeats held: 2
after repeats: 0 scheduled, 0 held
logged: 1
Stratum world "world3": its aquifer water and lava will not flow, because its generator options do not parse: Stratum's generator options take "blob", "settings" and "biomes" and nothing else, so "biome" would be ignored: {"blob":"/nonexistent/stratum.blob","biome":"minecraft:the_end"}
gone: 0 scheduled, 0 held, 1 logged
held again: 1
after unload: 0 scheduled, 0 held
after unload, as another generator's: 0 held
still in the extension for the Stratum world: yes
