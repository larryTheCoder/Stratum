--TEST--
The plugin schedules a block update at each liquid the engine marked, once, in Stratum worlds only
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
// PocketMine-MP is not installable here (ext/plugin/README.md), so the
// classes FluidUpdateScheduler touches are stand-ins with the signatures it
// calls, read from PocketMine-MP 5.44.4: World::getBlockAt(int, int, int),
// World::scheduleDelayedBlockUpdate(Vector3, int), World::getId/getSeed/
// getFolderName/getProvider()->getWorldData()->getGenerator/
// getGeneratorOptions, Block::getPosition(), Liquid::tickRate() (water 5,
// lava 30), ChunkEvent::getChunkX/getChunkZ, WorldEvent::getWorld,
// GeneratorManager::getInstance()->getGenerator(name)?->getGeneratorClass().
// The engine side is the real extension: the updates are the ones
// Dimension::encodeChunk left, taken through Stratum\takeFluidUpdates.

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

namespace pocketmine\world {
	use pocketmine\math\Vector3;

	class World{
		/** @var list<array{int, int, int, int}> */
		public array $scheduled = [];

		/** @param \Closure(int, int, int) : \pocketmine\block\Block $blockAt */
		public function __construct(
			private int $id,
			private string $generator,
			private string $generatorOptions,
			private int $seed,
			private \Closure $blockAt
		){}

		public function getId() : int{ return $this->id; }
		public function getSeed() : int{ return $this->seed; }
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
			$this->scheduled[] = [(int) $pos->x, (int) $pos->y, (int) $pos->z, $delay];
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
	require __DIR__ . "/../src/FluidUpdateScheduler.php";

	use pocketmine\block\Block;
	use pocketmine\block\Lava;
	use pocketmine\block\Water;
	use pocketmine\event\world\ChunkPopulateEvent;
	use pocketmine\event\world\WorldUnloadEvent;
	use pocketmine\math\Vector3;
	use pocketmine\world\World;
	use Stratum\PMMP\FluidUpdateScheduler;
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

	$scheduler = new FluidUpdateScheduler($logger);

	// A world another generator owns schedules nothing and takes nothing,
	// even with the same options and seed.
	$normal = new World(1, "normal", $options, $seed, $blockAt);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($normal, -3, 2));
	echo "normal world scheduled: ", count($normal->scheduled), "\n";

	// The Stratum world takes them: every liquid, at its own tick rate, in
	// the engine's order; the stone is left alone.
	$stratum = new World(2, "stratum", $options, $seed, $blockAt);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	echo "scheduled: ", count($stratum->scheduled) === count($expected) - 1 ? "all but the stone" : count($stratum->scheduled), "\n";
	$positions = array_map(fn($s) => [$s[0], $s[1], $s[2]], $stratum->scheduled);
	var_dump($positions === array_slice($expected, 1));
	echo "lava delay: ", $stratum->scheduled[0][3], "\n";
	echo "water delays: ", implode(",", array_unique(array_map(fn($s) => $s[3], array_slice($stratum->scheduled, 1)))), "\n";

	// Once: populating it again schedules nothing more, and a chunk nothing
	// was left for schedules nothing.
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, -3, 2));
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($stratum, 0, 0));
	echo "after repeats: ", count($stratum->scheduled) === count($expected) - 1 ? "unchanged" : "changed", "\n";

	// A world whose options no longer parse says so once, by name.
	$broken = new World(3, "stratum", GeneratorOptions::encode("/nonexistent/stratum.blob"), $seed, $blockAt);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($broken, -3, 2));
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($broken, -3, 2));
	echo "logged: ", count($logged), "\n";
	echo str_replace($blob, "<blob>", $logged[0]), "\n";

	// What a world's options say is read once, and forgotten at unload: the
	// same id, after an unload, is read afresh.
	$dimension->encodeChunk(-3, 2);
	$scheduler->onWorldUnload(new WorldUnloadEvent($stratum));
	$reused = new World(2, "normal", $options, $seed, $blockAt);
	$scheduler->onChunkPopulate(new ChunkPopulateEvent($reused, -3, 2));
	echo "after unload, as another generator's: ", count($reused->scheduled), "\n";
	echo "still held for the Stratum world: ", count(Stratum\takeFluidUpdates($blob, "minecraft:overworld", "minecraft:overworld", $seed, -3, 2)) === count($expected) ? "yes" : "no", "\n";

	unset($dimension);
	unlink($blob);
}
?>
--EXPECT--
marked: several
normal world scheduled: 0
scheduled: all but the stone
bool(true)
lava delay: 30
water delays: 5
after repeats: unchanged
logged: 1
Stratum world "world3": its aquifer water and lava will not flow, because its generator options no longer parse: Stratum cannot read this world's frozen pipeline at /nonexistent/stratum.blob
after unload, as another generator's: 0
still held for the Stratum world: yes
