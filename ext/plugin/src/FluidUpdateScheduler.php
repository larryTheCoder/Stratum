<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\block\Liquid;
use pocketmine\event\Listener;
use pocketmine\event\world\ChunkPopulateEvent;
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
 * and ticks each liquid its own tick rate after the chunk was populated:
 * the delay Liquid::onNearbyBlockChange() schedules when a neighbour
 * changes.
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
 * liquids released on a tick when none of its 3x3 is locked and every chunk
 * within two of it is loaded and populated — which PocketMine-MP never
 * populates again, so no population can lock that 3x3 from then on, and a
 * liquid not yet due can wait in PocketMine-MP's own scheduled-update
 * queue. A chunk at the edge of what is populated is held until the chunks
 * beyond it are populated too: this keeps only its coordinates and the
 * tick it was populated on, and its positions stay in the extension.
 * release() runs from the plugin's scheduler, which PocketMine-MP ticks
 * before it collects finished population tasks and before the world ticks
 * its scheduled updates. A flow under way then carries on as
 * PocketMine-MP's own liquids do, one scheduled update after another.
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
	 * World id => the world, its options, and its populated chunks whose
	 * liquids are not yet released: "x z" => [x, z, the server tick it was
	 * populated on]. The positions themselves stay in the extension, a
	 * dozen bytes each, until the chunk is released.
	 *
	 * @var array<int, array{World, GeneratorOptions, array<string, array{int, int, int}>}>
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
		$id = $world->getId();
		$this->held[$id] ??= [$world, $options, []];
		$this->held[$id][2][$chunkX . " " . $chunkZ] = [$chunkX, $chunkZ, $world->getServer()->getTick()];
	}

	/**
	 * Releases every held chunk that has settled (settledAround): takes its
	 * liquids from the extension and schedules each, after what is left of
	 * its tick rate since the chunk was populated. Called once a server
	 * tick, from the plugin's scheduler (Main).
	 */
	public function release() : void{
		foreach($this->held as $id => [$world, $options, $chunks]){
			if(!$world->isLoaded()){
				unset($this->held[$id]);
				continue;
			}
			$now = $world->getServer()->getTick();
			foreach($chunks as $key => [$chunkX, $chunkZ, $populated]){
				if(!self::settledAround($world, $chunkX, $chunkZ)){
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
						$world->scheduleDelayedBlockUpdate($block->getPosition(), max(0, $populated + $block->tickRate() - $now));
					}
				}
			}
			// Absent already if scheduling an update unloaded the world.
			if(($this->held[$id][2] ?? []) === []){
				unset($this->held[$id]);
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
	 * MONITOR, so it only sees an unload nothing cancelled. What a world
	 * held goes with it.
	 *
	 * @priority MONITOR
	 */
	public function onWorldUnload(WorldUnloadEvent $event) : void{
		unset($this->held[$event->getWorld()->getId()]);
	}

	/**
	 * Whether no population task can make PocketMine-MP discard its result
	 * for a block set in the 3x3 round ($chunkX, $chunkZ), now or later:
	 * none of that 3x3 is locked by one in flight, and every chunk within
	 * two of it is loaded and populated already — every chunk whose own
	 * population would lock part of that 3x3, and none of which PocketMine-MP
	 * populates again.
	 */
	private static function settledAround(World $world, int $chunkX, int $chunkZ) : bool{
		for($dx = -1; $dx <= 1; ++$dx){
			for($dz = -1; $dz <= 1; ++$dz){
				if($world->isChunkLocked($chunkX + $dx, $chunkZ + $dz)){
					return false;
				}
			}
		}
		for($dx = -2; $dx <= 2; ++$dx){
			for($dz = -2; $dz <= 2; ++$dz){
				// getChunk(), never loadChunk(): a chunk not in memory is
				// not settled, and is not read from disk to find out.
				$chunk = $world->getChunk($chunkX + $dx, $chunkZ + $dz);
				if($chunk === null || !$chunk->isPopulated()){
					return false;
				}
			}
		}
		return true;
	}
}
