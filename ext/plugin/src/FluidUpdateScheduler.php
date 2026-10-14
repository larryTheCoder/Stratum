<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\block\Liquid;
use pocketmine\event\Listener;
use pocketmine\event\world\ChunkLoadEvent;
use pocketmine\event\world\ChunkPopulateEvent;
use pocketmine\event\world\ChunkUnloadEvent;
use pocketmine\event\world\WorldUnloadEvent;
use pocketmine\world\World;
use function count;
use function max;

/**
 * Makes a Stratum world's aquifer water and lava flow once their chunk is in
 * the world, as they do in a world a Java server generated.
 *
 * A Java server keeps those positions in the chunk's PostProcessing lists
 * and ticks them once the chunk loads (spec Q8.1; SPEC section 11's Q8
 * entry). PocketMine-MP has no such list, and a generator cannot carry one
 * back with its chunk: it runs on a worker thread, and only the terrain
 * crosses back (FastChunkSerializer::serializeTerrain). So the engine leaves
 * each chunk's positions in the extension when a worker encodes it
 * (Stratum\Dimension::encodeChunk), and this takes them on the main thread —
 * Stratum\takeFluidUpdates, by the world's options and seed — once
 * PocketMine-MP has populated the chunk and the chunks round it (below),
 * and ticks each liquid its own tick rate after the chunk was held: the
 * delay Liquid::onNearbyBlockChange() schedules when a neighbour changes.
 *
 * Why the chunk's population and not its first load: PocketMine-MP generates
 * a chunk's neighbours to populate it, so a populated chunk's eight
 * neighbours all exist — and by the time its liquids are released (below),
 * every chunk within three of it does. A liquid flowing out of it reaches
 * ungenerated terrain, where World::setBlockAt() throws inside a scheduled
 * update, only by running on past all of those (read from 5.44.3, never
 * seen); one reaching a chunk unloaded meanwhile reads it as air
 * (World::getBlockAt) and may write into it. A chunk generated
 * only as a neighbour keeps its updates in the extension until it is
 * populated itself — through any number of worker restarts, because
 * StratumWorlds pins the world's state on the main thread while it is
 * loaded; which worlds are Stratum's, and with what options, is
 * StratumWorlds' to say.
 *
 * NEVER WHERE A POPULATION IS IN FLIGHT, OR STILL TO COME. A population
 * task locks the 3x3 chunks it populates (World::lockChunk), and any block
 * set in one of them while it runs (World::setBlockAt unlocks the chunk)
 * makes PocketMine-MP discard the task's whole result and populate again
 * (World::generateChunkCallback, "Discarding population result"). Ticking
 * the liquids as their chunks were populated, while the spawn area was
 * still generating, made PocketMine-MP throw away 3 to 56 populations a
 * run (SPEC section 11's M5 entry); waiting only until no chunk of the 3x3
 * was locked still lost 18 in one, to populations that started while the
 * flow went on. A flow sets blocks in its own chunk's 3x3 (water runs seven
 * blocks from a source), and a population locks that 3x3 only if it is of
 * a chunk within two of this one. So a populated chunk is held, and its
 * liquids released on a tick when every chunk within two of it is loaded
 * and populated and none of its 3x3 is locked — PocketMine-MP never
 * populates those chunks again, so no population can lock that 3x3 from
 * then on, and a liquid not yet due can wait in PocketMine-MP's own
 * scheduled-update queue. release() runs from the plugin's scheduler,
 * which PocketMine-MP ticks before it collects finished population tasks
 * and before the world ticks its scheduled updates. A flow under way then
 * carries on as PocketMine-MP's own liquids do, one scheduled update after
 * another.
 *
 * LOOKED AT ONLY WHEN SOMETHING ROUND IT CHANGES. Whether a held chunk has
 * settled changes only when a chunk within two of it is populated
 * (ChunkPopulateEvent) or loaded already populated (ChunkLoadEvent) — the
 * ways PocketMine-MP 5.44.3 makes a chunk loaded and populated — or when a
 * lock in its 3x3 goes. So a held chunk is looked at on the tick after such
 * an event, and not again while a chunk within two of it is missing or
 * unpopulated: a population there will announce itself. A lock is looked
 * at again every tick, but no population can hold one in its 3x3 once
 * every chunk within two is populated (a population locks round a chunk
 * not yet populated), so only another plugin's lock keeps a chunk looked
 * at. A chunk at the edge of what is populated, or one whose 5x5 is never
 * all loaded at once, therefore costs nothing per tick, however many there
 * are (it used to be looked at every tick: 10 ms a tick at 20,000 such
 * chunks, measured in the real server).
 *
 * HELD WHILE LOADED. A held chunk is let go when it unloads
 * (ChunkUnloadEvent), and held again when it is next loaded, populated,
 * from then on as though it had just been populated; so what is held in
 * PHP is bounded by the chunks loaded. Its positions stay in the extension
 * meanwhile — a dozen bytes each; over the 289 chunks round the smoke
 * world's spawn (seed 20260915), 101 a chunk on average and 1,346 at most,
 * about 1.2 KB and 16 KB — and so do those of a chunk generated only as a
 * neighbour: for as long as the world is loaded and pinned, its outbox
 * keeps every such chunk that has not settled, and grows with the terrain
 * generated round what has. It goes with the world's unload. A chunk
 * loaded populated that was never held, generated in an earlier run or
 * settled already, finds nothing there.
 *
 * What does not carry over: the updates live in memory, so a chunk generated
 * in one server run (or one load of the world) and first settled in a later
 * one is settled without them — its aquifer fluid stays as generated until
 * something next to it changes. And PocketMine-MP does not save scheduled
 * block updates, so a flow the server stops in the middle of stops there.
 * Both are as PocketMine-MP treats any liquid. A chunk a plugin replaces
 * with World::setChunk() announces nothing, so a held chunk next to it is
 * looked at again only at the next event round it.
 */
final class FluidUpdateScheduler implements Listener{

	/** settledAround(): released now. */
	private const SETTLED = 0;
	/** settledAround(): a chunk within two is missing or unpopulated; an event will say when that changes. */
	private const WAITING = 1;
	/** settledAround(): everything within two is populated, but a chunk of the 3x3 is locked; looked at every tick. */
	private const LOCKED = 2;

	/**
	 * World id => the world, its options, and its loaded populated chunks
	 * whose liquids are not yet released: "x z" => [x, z, the server tick it
	 * was held from]. The positions themselves stay in the extension, a
	 * dozen bytes each, until the chunk is released.
	 *
	 * @var array<int, array{World, GeneratorOptions, array<string, array{int, int, int}>}>
	 */
	private array $held = [];

	/**
	 * World id => "x z" => true: the held chunks release() looks at on its
	 * next run — those something round them changed for since it last
	 * looked, and those a lock alone holds back.
	 *
	 * @var array<int, array<string, true>>
	 */
	private array $due = [];

	public function __construct(
		private StratumWorlds $worlds
	){}

	public function onChunkPopulate(ChunkPopulateEvent $event) : void{
		$world = $event->getWorld();
		$options = $this->worlds->optionsFor($world);
		if($options === null){
			return;
		}
		$this->hold($world, $options, $event->getChunkX(), $event->getChunkZ());
	}

	/**
	 * A chunk loaded populated may be one of those whose settling waits on
	 * it, and may itself have been held before it was last unloaded; one
	 * loaded unpopulated settles nothing until its ChunkPopulateEvent.
	 *
	 * @priority MONITOR
	 */
	public function onChunkLoad(ChunkLoadEvent $event) : void{
		if(!$event->getChunk()->isPopulated()){
			return;
		}
		$world = $event->getWorld();
		$options = $this->worlds->optionsFor($world);
		if($options === null){
			return;
		}
		$this->hold($world, $options, $event->getChunkX(), $event->getChunkZ());
	}

	/**
	 * MONITOR, so it only sees an unload nothing cancelled. The chunk is let
	 * go — its positions stay in the extension — and held again when it is
	 * next loaded.
	 *
	 * @priority MONITOR
	 */
	public function onChunkUnload(ChunkUnloadEvent $event) : void{
		$id = $event->getWorld()->getId();
		if(!isset($this->held[$id])){
			return;
		}
		$key = $event->getChunkX() . " " . $event->getChunkZ();
		unset($this->held[$id][2][$key], $this->due[$id][$key]);
		if($this->held[$id][2] === []){
			unset($this->held[$id], $this->due[$id]);
		}
	}

	/**
	 * Releases every held chunk something round it changed for that has now
	 * settled (settledAround): takes its liquids from the extension and
	 * schedules each, after what is left of its tick rate since the chunk
	 * was held. Called once a server tick, from the plugin's scheduler
	 * (Main); with nothing due it does nothing.
	 */
	public function release() : void{
		foreach($this->due as $id => $keys){
			$entry = $this->held[$id] ?? null;
			if($entry === null){
				unset($this->due[$id]);
				continue;
			}
			[$world, $options] = $entry;
			if(!$world->isLoaded()){
				unset($this->held[$id], $this->due[$id]);
				continue;
			}
			$now = $world->getServer()->getTick();
			foreach($keys as $key => $_){
				// Read afresh each time: scheduling an update in an earlier
				// chunk may, in principle, have unloaded something.
				$chunk = $this->held[$id][2][$key] ?? null;
				if($chunk === null){
					unset($this->due[$id][$key]);
					continue;
				}
				[$chunkX, $chunkZ, $since] = $chunk;
				$state = self::settledAround($world, $chunkX, $chunkZ);
				if($state === self::LOCKED){
					continue;
				}
				unset($this->due[$id][$key]);
				if($state === self::WAITING){
					continue;
				}
				unset($this->held[$id][2][$key]);
				$updates = \Stratum\takeFluidUpdates(
					$options->blobPath,
					$options->noiseSettings,
					$options->biomeSource,
					$world->getSeed(),
					$chunkX,
					$chunkZ
				);
				foreach($updates as [$x, $y, $z]){
					$block = $world->getBlockAt($x, $y, $z);
					// Every position the engine marks held fluid when it was
					// generated; anything a plugin has since replaced is left
					// alone. Settled is for good, so a liquid not yet due can
					// wait in PocketMine-MP's own queue.
					if($block instanceof Liquid){
						$world->scheduleDelayedBlockUpdate($block->getPosition(), max(0, $since + $block->tickRate() - $now));
					}
				}
			}
			// Absent already if scheduling an update unloaded the world.
			if(($this->due[$id] ?? []) === []){
				unset($this->due[$id]);
			}
			if(isset($this->held[$id]) && $this->held[$id][2] === []){
				unset($this->held[$id], $this->due[$id]);
			}
		}
	}

	/**
	 * How many populated chunks are held, their liquids not yet released,
	 * in every world.
	 *
	 * @notHandler
	 */
	public function chunksHeld() : int{
		$count = 0;
		foreach($this->held as [, , $chunks]){
			$count += count($chunks);
		}
		return $count;
	}

	/**
	 * How many held chunks the next release() looks at, in every world.
	 *
	 * @notHandler
	 */
	public function chunksDue() : int{
		$count = 0;
		foreach($this->due as $keys){
			$count += count($keys);
		}
		return $count;
	}

	/**
	 * MONITOR, so it only sees an unload nothing cancelled. What a world
	 * held goes with it.
	 *
	 * @priority MONITOR
	 */
	public function onWorldUnload(WorldUnloadEvent $event) : void{
		$id = $event->getWorld()->getId();
		unset($this->held[$id], $this->due[$id]);
	}

	/**
	 * Holds chunk ($chunkX, $chunkZ) of $world from this tick, unless it is
	 * held already, and has release() look at every held chunk within two of
	 * it — itself included — on its next run.
	 */
	private function hold(World $world, GeneratorOptions $options, int $chunkX, int $chunkZ) : void{
		$id = $world->getId();
		$this->held[$id] ??= [$world, $options, []];
		$this->held[$id][2][$chunkX . " " . $chunkZ] ??= [$chunkX, $chunkZ, $world->getServer()->getTick()];
		for($dx = -2; $dx <= 2; ++$dx){
			for($dz = -2; $dz <= 2; ++$dz){
				$key = ($chunkX + $dx) . " " . ($chunkZ + $dz);
				if(isset($this->held[$id][2][$key])){
					$this->due[$id][$key] = true;
				}
			}
		}
	}

	/**
	 * Whether no population task can make PocketMine-MP discard its result
	 * for a block set in the 3x3 round ($chunkX, $chunkZ), now or later:
	 * every chunk within two of it is loaded and populated already — every
	 * chunk whose own population would lock part of that 3x3, and none of
	 * which PocketMine-MP populates again — and none of that 3x3 is locked.
	 *
	 * @return self::SETTLED|self::WAITING|self::LOCKED
	 */
	private static function settledAround(World $world, int $chunkX, int $chunkZ) : int{
		for($dx = -2; $dx <= 2; ++$dx){
			for($dz = -2; $dz <= 2; ++$dz){
				// getChunk(), never loadChunk(): a chunk not in memory is
				// not settled, and is not read from disk to find out.
				$chunk = $world->getChunk($chunkX + $dx, $chunkZ + $dz);
				if($chunk === null || !$chunk->isPopulated()){
					return self::WAITING;
				}
			}
		}
		for($dx = -1; $dx <= 1; ++$dx){
			for($dz = -1; $dz <= 1; ++$dz){
				if($world->isChunkLocked($chunkX + $dx, $chunkZ + $dz)){
					return self::LOCKED;
				}
			}
		}
		return self::SETTLED;
	}
}
