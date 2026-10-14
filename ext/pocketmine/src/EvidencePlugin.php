<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use pocketmine\event\Listener;
use pocketmine\event\world\WorldInitEvent;
use pocketmine\math\Vector3;
use pocketmine\plugin\PluginBase;
use function count;
use function file_get_contents;
use function is_array;
use function is_int;
use function is_string;
use function json_decode;

/**
 * Test tooling for tools/pmmp-dev, packed beside the Stratum plugin from
 * ext/pocketmine (plugin.yml there) and never part of it. It changes nothing
 * the plugin under test does. What it does is read from the
 * stratum-evidence.json the tool writes into the server directory:
 *
 *   {"world": "stratum_end", "spawn": [0, 70, 0]}
 *
 * "spawn": on the world's WorldInitEvent — fired by
 * WorldManager::generateWorld() after it writes the new world's level.dat
 * and before it orders the spawn area's population round the world's spawn
 * — sets that spawn. PocketMine-MP gives every new world (256, 70, 256), and
 * a pocketmine.yml worlds: entry cannot say otherwise; in the End that is
 * over the void, so the smoke's spawn area would hold nothing to compare.
 */
final class EvidencePlugin extends PluginBase implements Listener{

	private string $worldName = "";
	/** @var array{int, int, int}|null */
	private ?array $spawn = null;

	protected function onEnable() : void{
		$path = $this->getServer()->getDataPath() . "stratum-evidence.json";
		$settings = json_decode((string) @file_get_contents($path), associative: true);
		if(!is_array($settings) || !is_string($settings["world"] ?? null)){
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
		$this->getServer()->getPluginManager()->registerEvents($this, $this);
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
}
