<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\block\Liquid;
use pocketmine\event\Listener;
use pocketmine\event\world\ChunkPopulateEvent;
use pocketmine\event\world\WorldUnloadEvent;
use pocketmine\world\generator\GeneratorManager;
use pocketmine\world\generator\InvalidGeneratorOptionsException;
use pocketmine\world\World;
use function array_key_exists;

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
 * Stratum\takeFluidUpdates, by the world's options and seed — and schedules
 * a block update at each liquid, after that liquid's own tick rate: the same
 * call Liquid::onNearbyBlockChange() makes when a neighbour changes.
 *
 * Why the chunk's population and not its first load: PocketMine-MP generates
 * a chunk's neighbours to populate it, so a populated chunk's eight
 * neighbours all exist, and a liquid flowing out of it never reaches
 * ungenerated terrain, where World::setBlockAt() throws. A chunk generated
 * only as a neighbour keeps its updates in the extension until it is
 * populated itself.
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
	 * World id => its generator options, or null for a world Stratum does
	 * not generate. Read once per world: the options never change while it
	 * is loaded.
	 *
	 * @var array<int, GeneratorOptions|null>
	 */
	private array $worlds = [];

	public function __construct(
		private \Logger $logger
	){}

	public function onChunkPopulate(ChunkPopulateEvent $event) : void{
		$world = $event->getWorld();
		$options = $this->optionsFor($world);
		if($options === null){
			return;
		}
		$updates = \Stratum\takeFluidUpdates(
			$options->blobPath,
			$options->noiseSettings,
			$options->biomeParameterList,
			$world->getSeed(),
			$event->getChunkX(),
			$event->getChunkZ()
		);
		foreach($updates as [$x, $y, $z]){
			$block = $world->getBlockAt($x, $y, $z);
			// Every position the engine marks held fluid when it was
			// generated; anything a plugin has since replaced is left alone.
			if($block instanceof Liquid){
				$world->scheduleDelayedBlockUpdate($block->getPosition(), $block->tickRate());
			}
		}
	}

	/**
	 * MONITOR, so it only sees an unload nothing cancelled.
	 *
	 * @priority MONITOR
	 */
	public function onWorldUnload(WorldUnloadEvent $event) : void{
		unset($this->worlds[$event->getWorld()->getId()]);
	}

	private function optionsFor(World $world) : ?GeneratorOptions{
		$id = $world->getId();
		if(!array_key_exists($id, $this->worlds)){
			$this->worlds[$id] = null;
			$data = $world->getProvider()->getWorldData();
			$entry = GeneratorManager::getInstance()->getGenerator($data->getGenerator());
			if($entry !== null && $entry->getGeneratorClass() === StratumGenerator::class){
				try{
					$this->worlds[$id] = GeneratorOptions::parse($data->getGeneratorOptions());
				}catch(InvalidGeneratorOptionsException $e){
					// The world loaded with these options, so they parsed
					// then; most likely its frozen pipeline has since gone.
					// Said once, by name, rather than crashing the tick.
					$this->logger->error(
						"Stratum world \"" . $world->getFolderName() . "\": its aquifer water and lava will not flow, " .
						"because its generator options no longer parse: " . $e->getMessage()
					);
				}
			}
		}
		return $this->worlds[$id];
	}
}
