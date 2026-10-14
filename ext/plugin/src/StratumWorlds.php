<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\event\Listener;
use pocketmine\event\world\WorldLoadEvent;
use pocketmine\event\world\WorldUnloadEvent;
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
 * A pin that fails is logged, by name, and not thrown: an exception out of a
 * WorldLoadEvent handler would take the server down, and the world's options
 * passed GeneratorOptions::validate() moments earlier, so the likeliest
 * cause is a blob replaced in between — its generators will then refuse it
 * themselves.
 */
final class StratumWorlds implements Listener{

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

	public function __construct(
		private \Logger $logger
	){}

	/**
	 * LOWEST, so the world is pinned before any other plugin's handler can
	 * ask for one of its chunks.
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
			$this->logger->error(
				"Stratum world \"" . $world->getFolderName() . "\": its state could not be pinned on the main thread, " .
				"so its generators will open its frozen pipeline themselves: " . $e->getMessage()
			);
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
}
