<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\event\Listener;
use pocketmine\event\world\WorldLoadEvent;
use pocketmine\event\world\WorldUnloadEvent;
use pocketmine\world\ChunkLockId;
use pocketmine\world\generator\GeneratorManager;
use pocketmine\world\generator\InvalidGeneratorOptionsException;
use pocketmine\world\World;
use function array_key_exists;

/**
 * Which loaded worlds Stratum generates, with what options, and the pin that
 * holds each one's engine state on the main thread from world load to world
 * unload.
 *
 * WHY PIN. A world's state in the extension — its compiled dimension, and
 * the fluid updates its generators have left for chunks not yet populated
 * (FluidUpdateScheduler) — lives as long as something holds it, and
 * otherwise only its generators do, one per generation worker.
 * PocketMine-MP's memory manager shuts idle workers down (its periodic
 * garbage collection), and every generator on them goes too: with the last,
 * the held fluid updates were lost, and the next generator read the blob
 * from disk afresh, whatever that file held by then. Pinned
 * (Stratum\pinWorld), the updates outlive every worker restart, and every
 * generator — a restarted one included — gets the state the world loaded
 * with, without the file being read again.
 *
 * A PIN THAT FAILS. The pin is the same full open a generator makes, so a
 * world whose pin fails would fail on its first generation worker, and an
 * exception there takes PocketMine-MP down (measured). Its options passed
 * GeneratorOptions::validate() moments before, so the likeliest cause is a
 * blob replaced in between. Such a world is logged, by name, and kept off
 * the workers until it can be unloaded — and neither is done by throwing,
 * since an exception out of a WorldLoadEvent handler takes the server down
 * too. What is safe here was read from PocketMine-MP 5.44.3:
 *
 *  - The world cannot be unloaded inside the event. WorldManager::
 *    generateWorld() fires WorldLoadEvent and then, for a new world, orders
 *    the population of every chunk within eight of its spawn on the same
 *    World object; unloaded, that World's provider is closed, and the first
 *    loadChunk() throws on the main thread. And every other plugin's
 *    WorldLoadEvent handler, at a later priority, would be handed an
 *    unloaded world.
 *  - But a population whose 3x3 holds a locked chunk is queued, not handed
 *    to a worker (World::internalOrderChunkPopulation); no generator is
 *    registered on a worker until a population is (AsyncGeneratorExecutor::
 *    populate). So every chunk within eight of the spawn is locked here,
 *    under a lock of this class's own, and generateWorld()'s orders wait.
 *  - On the next tick — this plugin's task scheduler, which the server runs
 *    before anything else in a tick — the world is force-unloaded, and its
 *    unload rejects every population still queued: none ever reached a
 *    worker. A default world cannot be unloaded and leave a server players
 *    can join, so the server is stopped instead, by name, as PocketMine-MP
 *    stops when its default world fails to load.
 *
 * What this cannot hold back: a population some other code orders, outside
 * the locked spawn area, between the failed pin and the next tick.
 */
final class StratumWorlds implements Listener{

	/**
	 * How far round its spawn a world whose pin failed is locked against
	 * population, in chunks: WorldManager::generateWorld() orders the spawn
	 * area round the spawn chunk with ChunkSelector::selectChunks(8, ...),
	 * whose chunks are at most 8 away on each axis.
	 */
	public const HELD_RADIUS = 8;

	/**
	 * World id => its generator options, or null for a world Stratum does
	 * not generate. Read once per world: the options never change while it
	 * is loaded.
	 *
	 * @var array<int, GeneratorOptions|null>
	 */
	private array $options = [];

	/**
	 * World id => the seed it is pinned under, beside its options above.
	 *
	 * @var array<int, int>
	 */
	private array $pinned = [];

	/**
	 * @param \Closure $nextTick runs the closure it is given once, on the
	 * main thread, on the next server tick (Main: the plugin's task
	 * scheduler).
	 * @phpstan-param \Closure(\Closure() : void) : void $nextTick
	 */
	public function __construct(
		private \Logger $logger,
		private \Closure $nextTick
	){}

	/**
	 * LOWEST, so the world is pinned — or held off generation — before any
	 * other plugin's handler can ask for one of its chunks.
	 *
	 * @priority LOWEST
	 */
	public function onWorldLoad(WorldLoadEvent $event) : void{
		$world = $event->getWorld();
		$options = $this->optionsFor($world);
		$id = $world->getId();
		if($options === null || array_key_exists($id, $this->pinned)){
			return;
		}
		try{
			\Stratum\pinWorld($options->blobPath, $options->noiseSettings, $options->biomeSource, $world->getSeed());
			$this->pinned[$id] = $world->getSeed();
		}catch(\Stratum\GenerationException $e){
			$this->holdOffAndUnload($world, $e->getMessage());
		}
	}

	/**
	 * MONITOR, so it only sees an unload nothing cancelled.
	 *
	 * @priority MONITOR
	 */
	public function onWorldUnload(WorldUnloadEvent $event) : void{
		$this->forget($event->getWorld()->getId());
	}

	/**
	 * Unpins every world this pinned, as the plugin is disabled.
	 */
	public function unpinAll() : void{
		foreach($this->pinned as $id => $_){
			$this->forget($id);
		}
	}

	/**
	 * $world's generator options if Stratum generates it, null otherwise.
	 * A Stratum world whose options do not parse — which PocketMine-MP,
	 * having validated them at load, should never present — is logged once,
	 * by name, and treated as not Stratum's, rather than thrown out of an
	 * event handler.
	 *
	 * @notHandler
	 */
	public function optionsFor(World $world) : ?GeneratorOptions{
		$id = $world->getId();
		if(!array_key_exists($id, $this->options)){
			$this->options[$id] = null;
			$data = $world->getProvider()->getWorldData();
			$entry = GeneratorManager::getInstance()->getGenerator($data->getGenerator());
			if($entry !== null && $entry->getGeneratorClass() === StratumGenerator::class){
				try{
					$this->options[$id] = GeneratorOptions::parse($data->getGeneratorOptions());
				}catch(InvalidGeneratorOptionsException $e){
					$this->logger->error(
						"Stratum world \"" . $world->getFolderName() . "\": its aquifer water and lava will not flow, " .
						"because its generator options do not parse: " . $e->getMessage()
					);
				}
			}
		}
		return $this->options[$id];
	}

	/**
	 * Whether $world is pinned.
	 *
	 * @notHandler
	 */
	public function isPinned(World $world) : bool{
		return array_key_exists($world->getId(), $this->pinned);
	}

	private function forget(int $id) : void{
		$options = $this->options[$id] ?? null;
		if($options !== null && array_key_exists($id, $this->pinned)){
			\Stratum\unpinWorld($options->blobPath, $options->noiseSettings, $options->biomeSource, $this->pinned[$id]);
		}
		unset($this->options[$id], $this->pinned[$id]);
	}

	/**
	 * $world could not be pinned, so it cannot generate: keeps every chunk
	 * of its spawn area from reaching a generation worker, and unloads it on
	 * the next tick — or stops the server, if it is the default world. The
	 * class doc says why this way and not another.
	 */
	private function holdOffAndUnload(World $world, string $why) : void{
		$name = $world->getFolderName();
		$this->logger->error(
			"Stratum world \"" . $name . "\" cannot generate: its state could not be pinned on the main thread (" . $why . "). " .
			"No chunk of it is handed to a generation worker, where the same refusal would take the server down, " .
			"and it is unloaded on the next tick"
		);
		$lock = new ChunkLockId();
		$spawn = $world->getSpawnLocation();
		// Chunk coordinates, as PocketMine-MP takes them: >> 4 floors.
		$spawnX = $spawn->getFloorX() >> 4;
		$spawnZ = $spawn->getFloorZ() >> 4;
		for($x = $spawnX - self::HELD_RADIUS; $x <= $spawnX + self::HELD_RADIUS; ++$x){
			for($z = $spawnZ - self::HELD_RADIUS; $z <= $spawnZ + self::HELD_RADIUS; ++$z){
				// One already locked is a population already in flight, which
				// nothing here can recall; lockChunk() would throw on it.
				if(!$world->isChunkLocked($x, $z)){
					$world->lockChunk($x, $z, $lock);
				}
			}
		}
		($this->nextTick)(function() use ($world, $name) : void{
			if(!$world->isLoaded()){
				return;
			}
			$server = $world->getServer();
			$manager = $server->getWorldManager();
			if($manager->getDefaultWorld() === $world){
				$this->logger->emergency(
					"Stratum world \"" . $name . "\" is the default world and cannot generate (its state could not be pinned); " .
					"stopping the server rather than run without one"
				);
				$server->shutdown();
				return;
			}
			$manager->unloadWorld($world, true);
		});
	}
}
