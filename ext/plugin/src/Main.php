<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\plugin\PluginBase;
use pocketmine\world\generator\GeneratorManager;
use pocketmine\world\generator\InvalidGeneratorOptionsException;

/**
 * Registers Stratum as a PocketMine-MP generator, the listener that holds
 * each Stratum world's engine state on the main thread while it is loaded
 * (StratumWorlds), and the one that makes its aquifer fluids flow
 * (FluidUpdateScheduler).
 *
 * Registration happens in onLoad() rather than onEnable() because worlds are
 * loaded between the two enable phases: a world configured with
 * generator=stratum is opened before any POSTWORLD plugin is enabled, and an
 * unregistered generator name does not merely fail that world — it aborts
 * server startup.
 */
final class Main extends PluginBase{

	private ?StratumWorlds $worlds = null;

	protected function onLoad() : void{
		GeneratorManager::getInstance()->addGenerator(
			StratumGenerator::class,
			"stratum",
			// The contract is to RETURN the exception, not throw it: the
			// caller throws it, and PocketMine-MP reports it as invalid
			// generator options rather than as a plugin crash. Called on the
			// main thread as each world loads, so this is where a world that
			// cannot generate is refused: validate() opens and compiles it
			// in full, and keeps nothing. On a worker, the same refusal
			// would take the server down.
			static function(string $generatorOptions) : ?InvalidGeneratorOptionsException{
				return GeneratorOptions::validate($generatorOptions);
			}
			// $overwrite and $fast are left at their defaults on purpose:
			// $fast=true would run C++ generation on the main thread, and the
			// parameter does not exist before PocketMine-MP 5.30.
		);
	}

	/**
	 * Listeners can only be registered once the plugin is enabled. Enabling
	 * at STARTUP is still before any world loads, so every Stratum world is
	 * pinned as it loads and no chunk is populated before this is listening.
	 */
	protected function onEnable() : void{
		$this->worlds = new StratumWorlds($this->getLogger());
		$pluginManager = $this->getServer()->getPluginManager();
		$pluginManager->registerEvents($this->worlds, $this);
		$pluginManager->registerEvents(new FluidUpdateScheduler($this->worlds), $this);
	}

	protected function onDisable() : void{
		$this->worlds?->unpinAll();
		$this->worlds = null;
	}
}
