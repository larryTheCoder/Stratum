<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\plugin\PluginBase;
use pocketmine\world\generator\GeneratorManager;
use pocketmine\world\generator\InvalidGeneratorOptionsException;

/**
 * Registers Stratum as a PocketMine-MP generator.
 *
 * Registration happens in onLoad() rather than onEnable() because worlds are
 * loaded between the two enable phases: a world configured with
 * generator=stratum is opened before any POSTWORLD plugin is enabled, and an
 * unregistered generator name does not merely fail that world — it aborts
 * server startup.
 */
final class Main extends PluginBase{

	protected function onLoad() : void{
		GeneratorManager::getInstance()->addGenerator(
			StratumGenerator::class,
			"stratum",
			// The contract is to RETURN the exception, not throw it: the
			// caller throws it, and PocketMine-MP reports it as invalid
			// generator options rather than as a plugin crash.
			static function(string $generatorOptions) : ?InvalidGeneratorOptionsException{
				return GeneratorOptions::validate($generatorOptions);
			}
			// $overwrite and $fast are left at their defaults on purpose:
			// $fast=true would run C++ generation on the main thread, and the
			// parameter does not exist before PocketMine-MP 5.30.
		);
	}
}
