<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\block\Liquid;
use pocketmine\event\Listener;
use pocketmine\event\world\ChunkPopulateEvent;
use pocketmine\event\world\WorldUnloadEvent;
use pocketmine\world\World;
use function count;

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
 * Stratum\takeFluidUpdates, by the world's options and seed — when
 * PocketMine-MP announces the chunk populated, and ticks each liquid once
 * its own tick rate has passed: the delay Liquid::onNearbyBlockChange()
 * schedules when a neighbour changes.
 *
 * Why the chunk's population and not its first load: PocketMine-MP generates
 * a chunk's neighbours to populate it, so a populated chunk's eight
 * neighbours all exist, and a liquid flowing out of it never reaches
 * ungenerated terrain, where World::setBlockAt() throws. A chunk generated
 * only as a neighbour keeps its updates in the extension until it is
 * populated itself — through any number of worker restarts, because
 * StratumWorlds pins the world's state on the main thread while it is
 * loaded; which worlds are Stratum's, and with what options, is
 * StratumWorlds' to say.
 *
 * NEVER WHILE A POPULATION AROUND IT IS IN FLIGHT. A population task locks
 * the 3x3 chunks it populates (World::lockChunk), and any block set in one
 * of them while it runs (World::setBlockAt unlocks the chunk) makes
 * PocketMine-MP discard the task's whole result and populate again
 * (World::generateChunkCallback, "Discarding population result"). A
 * liquid's first flow sets blocks in its own chunk and the four beside it,
 * so a chunk's held updates are released only on a tick when none of the
 * 3x3 around it is locked — that is, when no population task within two
 * chunks of it is in flight. Ticking them as the spawn area generated made
 * PocketMine-MP throw away 3 to 56 populations a run (SPEC section 11's M5
 * entry). release() runs from the plugin's scheduler, which PocketMine-MP
 * ticks before it collects finished population tasks — the only place new
 * ones start in a tick — and before the world ticks its scheduled updates;
 * so a liquid released there is ticked that same tick (a delay of 0), with
 * no population able to start in between except one a finished task's
 * completion orders. A flow already under way then carries on as
 * PocketMine-MP's own liquids do, one scheduled update after another,
 * whatever is populating near it.
 *
 * What does not carry over: the updates live in memory, so a chunk generated
 * in one server run and first populated in a later one is populated without
 * them — its aquifer fluid stays as generated until something next to it
 * changes. And PocketMine-MP does not save scheduled block updates, so a
 * flow the server stops in the middle of stops there. Both are as
 * PocketMine-MP treats any liquid.
 */
final class FluidUpdateScheduler implements Listener{

	/**
	 * World id => the world and its held chunks: "x z" => [x, z, updates],
	 * each update [x, y, z, the server tick it is due].
	 *
	 * @var array<int, array{World, array<string, array{int, int, list<array{int, int, int, int}>}>}>
	 */
	private array $held = [];

	public function __construct(
		private StratumWorlds $worlds
	){}

	public function onChunkPopulate(ChunkPopulateEvent $event) : void{
		$world = $event->getWorld();
		$options = $this->worlds->optionsFor($world);
		if($options === null){
			return;
		}
		$chunkX = $event->getChunkX();
		$chunkZ = $event->getChunkZ();
		$updates = \Stratum\takeFluidUpdates(
			$options->blobPath,
			$options->noiseSettings,
			$options->biomeSource,
			$world->getSeed(),
			$chunkX,
			$chunkZ
		);
		$now = $world->getServer()->getTick();
		$held = [];
		foreach($updates as [$x, $y, $z]){
			$block = $world->getBlockAt($x, $y, $z);
			// Every position the engine marks held fluid when it was
			// generated; anything a plugin has since replaced is left alone.
			if($block instanceof Liquid){
				$held[] = [$x, $y, $z, $now + $block->tickRate()];
			}
		}
		if($held !== []){
			$id = $world->getId();
			$this->held[$id][0] = $world;
			$this->held[$id][1][$chunkX . " " . $chunkZ] = [$chunkX, $chunkZ, $held];
		}
	}

	/**
	 * Ticks every held liquid that is due, in every chunk no population
	 * task is in flight around. Called once a server tick, from the
	 * plugin's scheduler (Main).
	 */
	public function release() : void{
		foreach($this->held as $id => [$world, $chunks]){
			if(!$world->isLoaded()){
				unset($this->held[$id]);
				continue;
			}
			$now = $world->getServer()->getTick();
			foreach($chunks as $key => [$chunkX, $chunkZ, $updates]){
				if(self::populationInFlightAround($world, $chunkX, $chunkZ)){
					continue;
				}
				$left = [];
				foreach($updates as $update){
					[$x, $y, $z, $due] = $update;
					if($due > $now){
						$left[] = $update;
						continue;
					}
					$block = $world->getBlockAt($x, $y, $z);
					if($block instanceof Liquid){
						$world->scheduleDelayedBlockUpdate($block->getPosition(), 0);
					}
				}
				if($left === []){
					unset($this->held[$id][1][$key]);
				}else{
					$this->held[$id][1][$key][2] = $left;
				}
			}
			if($this->held[$id][1] === []){
				unset($this->held[$id]);
			}
		}
	}

	/**
	 * How many chunks hold updates not yet released, in every world.
	 *
	 * @notHandler
	 */
	public function chunksHeld() : int{
		$count = 0;
		foreach($this->held as [, $chunks]){
			$count += count($chunks);
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
		unset($this->held[$event->getWorld()->getId()]);
	}

	/**
	 * Whether any chunk of the 3x3 around ($chunkX, $chunkZ) is locked: by
	 * a population task in flight, whose result a block set there would
	 * make PocketMine-MP discard.
	 */
	private static function populationInFlightAround(World $world, int $chunkX, int $chunkZ) : bool{
		for($dx = -1; $dx <= 1; ++$dx){
			for($dz = -1; $dz <= 1; ++$dz){
				if($world->isChunkLocked($chunkX + $dx, $chunkZ + $dz)){
					return true;
				}
			}
		}
		return false;
	}
}
