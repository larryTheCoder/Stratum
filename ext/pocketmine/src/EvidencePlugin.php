<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use pocketmine\command\Command;
use pocketmine\command\CommandSender;
use pocketmine\event\Listener;
use pocketmine\event\world\ChunkLoadEvent;
use pocketmine\event\world\ChunkPopulateEvent;
use pocketmine\event\world\WorldInitEvent;
use pocketmine\event\world\WorldLoadEvent;
use pocketmine\math\Vector3;
use pocketmine\player\ChunkSelector;
use pocketmine\plugin\PluginBase;
use pocketmine\scheduler\AsyncPool;
use pocketmine\scheduler\ClosureTask;
use pocketmine\world\World;
use function array_map;
use function count;
use function file_get_contents;
use function file_put_contents;
use function implode;
use function is_array;
use function is_bool;
use function is_int;
use function is_string;
use function json_decode;
use function json_encode;
use function max;
use function time;
use const JSON_THROW_ON_ERROR;

/**
 * Test tooling for tools/pmmp-dev, packed beside the Stratum plugin from
 * ext/pocketmine (plugin.yml there) and never part of it. It changes nothing
 * the plugin under test does; it reports what PocketMine-MP itself did, from
 * inside the server, and does two things on the tool's say-so. What it does
 * is read from the stratum-evidence.json the tool writes into the server
 * directory:
 *
 *   {"world": "stratum_end", "spawn": [0, 64, 0], "observe": false,
 *    "report": "<server dir>/stratum-evidence-report.json"}
 *
 * "spawn": on the world's WorldInitEvent — fired by
 * WorldManager::generateWorld() after it writes the new world's level.dat
 * and before it orders the spawn area's population round the world's spawn
 * — sets that spawn. PocketMine-MP gives every new world (256, 70, 256), and
 * a pocketmine.yml worlds: entry cannot say otherwise; in the End that is
 * over the void, so the smoke's spawn area would hold nothing to compare.
 *
 * "observe": what tools/pmmp-dev observe reads (its usage says what it
 * proves):
 *  - the world's WorldLoadEvent, at MONITOR (after StratumWorlds pinned it
 *    at LOWEST): how many of its chunks are loaded then, and whether the
 *    Stratum plugin's StratumWorlds records it pinned — read through
 *    reflection, since the plugin offers no API for it. That is the
 *    plugin's own record: the extension offers no way to ask whether a
 *    world is pinned without changing its pins, so what shows the pin
 *    itself held is the fluid updates delivered after the shutdown, below;
 *  - its first ChunkLoadEvent;
 *  - `stratumevidence status`: the async pool's running workers, when they
 *    were last given a task (AsyncPoolWorkerEntry::$lastUsed, which
 *    AsyncPool::shutdownUnusedWorkers() compares with 300 seconds ago),
 *    and the pin;
 *  - `stratumevidence populate`: orders the population of every chunk next
 *    to the spawn area that is generated and not populated — chunks the
 *    old workers generated, as neighbours, before they were shut down — and
 *    for each, the block updates scheduled while its ChunkPopulateEvent ran
 *    (the world's scheduled-update index, by reflection, before the first
 *    handler and after the last) and, from then on, every position in it
 *    that is ever in that index, checked each tick, so an update scheduled
 *    later than the event is seen too.
 * Each is logged as it happens and written to "report" when the plugin is
 * disabled, as the server stops.
 */
final class EvidencePlugin extends PluginBase implements Listener{

	private string $worldName = "";
	/** @var array{int, int, int}|null */
	private ?array $spawn = null;
	private bool $observe = false;
	private string $reportPath = "";

	/** @var array<string, mixed> */
	private array $report = ["status" => [], "populated" => []];
	/** @var array<int, true> chunk hash => true: chunks ordered populated by `populate` */
	private array $watched = [];
	/** @var array<int, int> chunk hash => the scheduled updates before its ChunkPopulateEvent's handlers ran */
	private array $before = [];
	/** @var array<int, array<int, true>> chunk hash => block hash => true: every position seen in the scheduled-update index */
	private array $seen = [];
	private bool $tracking = false;

	protected function onEnable() : void{
		$path = $this->getServer()->getDataPath() . "stratum-evidence.json";
		$settings = json_decode((string) @file_get_contents($path), associative: true);
		if(!is_array($settings) || !is_string($settings["world"] ?? null) || !is_bool($settings["observe"] ?? null) ||
			!is_string($settings["report"] ?? null)){
			$this->getLogger()->error("no usable " . $path . ": this plugin is tools/pmmp-dev's, which writes it");
			return;
		}
		$spawn = $settings["spawn"] ?? null;
		if($spawn !== null && !(is_array($spawn) && count($spawn) === 3 && is_int($spawn[0]) && is_int($spawn[1]) && is_int($spawn[2]))){
			$this->getLogger()->error($path . "'s \"spawn\" is not [x, y, z]");
			return;
		}
		$this->worldName = $settings["world"];
		$this->spawn = $spawn;
		$this->observe = $settings["observe"];
		$this->reportPath = $settings["report"];
		$this->getServer()->getPluginManager()->registerEvents($this, $this);
	}

	protected function onDisable() : void{
		if($this->reportPath === ""){
			return;
		}
		$this->track();
		foreach($this->watched as $hash => $_){
			World::getXZ($hash, $chunkX, $chunkZ);
			$seen = [];
			foreach($this->seen[$hash] ?? [] as $blockHash => $_seen){
				World::getBlockXYZ($blockHash, $x, $y, $z);
				$seen[] = [$x, $y, $z];
			}
			$this->report["populated"][$chunkX . "," . $chunkZ]["seen"] = $seen;
		}
		file_put_contents($this->reportPath, json_encode($this->report, JSON_THROW_ON_ERROR) . "\n");
	}

	/**
	 * @priority MONITOR
	 */
	public function onWorldInit(WorldInitEvent $event) : void{
		$world = $event->getWorld();
		if($this->spawn === null || $world->getFolderName() !== $this->worldName){
			return;
		}
		[$x, $y, $z] = $this->spawn;
		$world->setSpawnLocation(new Vector3($x, $y, $z));
		$this->getLogger()->info("world \"" . $this->worldName . "\" created; its spawn set to " . $x . " " . $y . " " . $z .
			" before its spawn area is ordered");
	}

	/**
	 * MONITOR: after StratumWorlds' LOWEST handler has pinned the world.
	 *
	 * @priority MONITOR
	 */
	public function onWorldLoad(WorldLoadEvent $event) : void{
		$world = $event->getWorld();
		if(!$this->observe || $world->getFolderName() !== $this->worldName){
			return;
		}
		$loaded = count($world->getLoadedChunks());
		$pinned = $this->pinned($world);
		$this->report["worldLoad"] = ["loadedChunks" => $loaded, "pinned" => $pinned];
		$this->getLogger()->info("WorldLoadEvent for world \"" . $this->worldName . "\": " . $loaded . " chunk(s) loaded, pinned by StratumWorlds' record: " .
			self::yesNo($pinned));
	}

	/**
	 * @priority MONITOR
	 */
	public function onChunkLoad(ChunkLoadEvent $event) : void{
		$world = $event->getWorld();
		if(!$this->observe || isset($this->report["firstChunkLoad"]) || $world->getFolderName() !== $this->worldName){
			return;
		}
		$this->report["firstChunkLoad"] = ["chunk" => [$event->getChunkX(), $event->getChunkZ()], "new" => $event->isNewChunk()];
		$this->getLogger()->info("first ChunkLoadEvent for world \"" . $this->worldName . "\": chunk " . $event->getChunkX() . "," .
			$event->getChunkZ() . ($event->isNewChunk() ? ", newly generated" : ", from disk"));
	}

	/**
	 * @priority LOWEST
	 */
	public function beforePopulateHandlers(ChunkPopulateEvent $event) : void{
		$hash = World::chunkHash($event->getChunkX(), $event->getChunkZ());
		if(isset($this->watched[$hash])){
			$this->before[$hash] = count($this->scheduledUpdates($event->getWorld()));
		}
	}

	/**
	 * @priority MONITOR
	 */
	public function afterPopulateHandlers(ChunkPopulateEvent $event) : void{
		$hash = World::chunkHash($event->getChunkX(), $event->getChunkZ());
		if(!isset($this->watched[$hash], $this->before[$hash])){
			return;
		}
		$scheduled = count($this->scheduledUpdates($event->getWorld())) - $this->before[$hash];
		$key = $event->getChunkX() . "," . $event->getChunkZ();
		$this->report["populated"][$key]["scheduledByHandlers"] = $scheduled;
		$this->track();
		$this->getLogger()->info("ChunkPopulateEvent for chunk " . $key . ": " . $scheduled .
			" block update(s) scheduled by its handlers");
	}

	public function onCommand(CommandSender $sender, Command $command, string $label, array $args) : bool{
		$world = $this->observe ? $this->getServer()->getWorldManager()->getWorldByName($this->worldName) : null;
		if($world === null){
			$this->getLogger()->error("stratumevidence: world \"" . $this->worldName . "\" is not loaded, or observing is off");
			return true;
		}
		return match($args[0] ?? ""){
			"status" => $this->status($world),
			"populate" => $this->populate($world),
			default => false
		};
	}

	private function status(World $world) : bool{
		$pool = $this->getServer()->getAsyncPool();
		/** @var array<int, \pocketmine\scheduler\AsyncPoolWorkerEntry> $entries */
		$entries = (new \ReflectionProperty(AsyncPool::class, "workers"))->getValue($pool);
		$lastUsed = 0;
		$queued = 0;
		foreach($entries as $entry){
			$lastUsed = max($lastUsed, $entry->lastUsed);
			$queued += $entry->tasks->count();
		}
		$running = $pool->getRunningWorkers();
		$pinned = $this->pinned($world);
		$this->report["status"][] = ["runningWorkers" => $running, "poolSize" => $pool->getSize(), "lastUsed" => $lastUsed,
			"queuedTasks" => $queued, "pinned" => $pinned, "time" => time()];
		$this->getLogger()->info("status: async workers running [" . implode(",", $running) . "] of " . $pool->getSize() .
			", " . $queued . " task(s) queued, last given a task at " . $lastUsed . ", now " . time() .
			"; world \"" . $this->worldName . "\" pinned by StratumWorlds' record: " . self::yesNo($pinned));
		return true;
	}

	private function populate(World $world) : bool{
		$spawn = $world->getSpawnLocation();
		$centerX = $spawn->getFloorX() >> 4;
		$centerZ = $spawn->getFloorZ() >> 4;
		$spawnArea = [];
		foreach((new ChunkSelector())->selectChunks(8, $centerX, $centerZ) as $hash){
			$spawnArea[$hash] = true;
		}
		$targets = [];
		foreach((new ChunkSelector())->selectChunks(9, $centerX, $centerZ) as $hash){
			World::getXZ($hash, $chunkX, $chunkZ);
			if(!isset($spawnArea[$hash]) && $world->isChunkGenerated($chunkX, $chunkZ) && !$world->isChunkPopulated($chunkX, $chunkZ)){
				$targets[$hash] = [$chunkX, $chunkZ];
			}
		}
		$this->report["ordered"] = count($targets);
		$this->getLogger()->info("populate: ordering the population of " . count($targets) .
			" chunk(s) next to the spawn area, generated and not populated: " .
			implode(" ", array_map(static fn(array $c) : string => $c[0] . "," . $c[1], $targets)));
		if(count($targets) === 0){
			return true;
		}
		if(!$this->tracking){
			$this->tracking = true;
			$this->getScheduler()->scheduleRepeatingTask(new ClosureTask(function() : void{
				$this->track();
			}), 1);
		}
		$done = 0;
		$total = count($targets);
		foreach($targets as $hash => [$chunkX, $chunkZ]){
			$this->watched[$hash] = true;
			$world->orderChunkPopulation($chunkX, $chunkZ, null)->onCompletion(
				function() use (&$done, $total) : void{
					if(++$done === $total){
						$this->report["completed"] = $done;
						$this->getLogger()->info("populate: all " . $total . " chunk(s) populated");
					}
				},
				function() use ($chunkX, $chunkZ) : void{
					$this->getLogger()->error("populate: the population of chunk " . $chunkX . "," . $chunkZ . " failed");
				}
			);
		}
		return true;
	}

	/**
	 * Notes every position in a watched chunk that has a block update
	 * scheduled now.
	 */
	private function track() : void{
		if(count($this->watched) === 0){
			return;
		}
		$world = $this->getServer()->getWorldManager()->getWorldByName($this->worldName);
		if($world === null){
			return;
		}
		foreach($this->scheduledUpdates($world) as $blockHash => $_){
			World::getBlockXYZ($blockHash, $x, $y, $z);
			$chunk = World::chunkHash($x >> 4, $z >> 4);
			if(isset($this->watched[$chunk])){
				$this->seen[$chunk][$blockHash] = true;
			}
		}
	}

	/**
	 * The world's scheduled block updates, block hash => delay: World's own
	 * index of its queue, which scheduleDelayedBlockUpdate() fills.
	 *
	 * @return array<int, int>
	 */
	private function scheduledUpdates(World $world) : array{
		return (new \ReflectionProperty(World::class, "scheduledBlockUpdateQueueIndex"))->getValue($world);
	}

	/**
	 * Whether the Stratum plugin's StratumWorlds records $world pinned, or null
	 * if that cannot be read (the plugin is not loaded, or no longer keeps
	 * its StratumWorlds where this looks).
	 */
	private function pinned(World $world) : ?bool{
		$plugin = $this->getServer()->getPluginManager()->getPlugin("Stratum");
		if(!$plugin instanceof \Stratum\PMMP\Main){
			return null;
		}
		$worlds = (new \ReflectionProperty(\Stratum\PMMP\Main::class, "worlds"))->getValue($plugin);
		return $worlds instanceof \Stratum\PMMP\StratumWorlds ? $worlds->isPinned($world) : null;
	}

	private static function yesNo(?bool $value) : string{
		return $value === null ? "cannot tell" : ($value ? "yes" : "no");
	}
}
