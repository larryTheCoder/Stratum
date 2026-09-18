<?php

declare(strict_types=1);

namespace Stratum\PMMP;

use pocketmine\block\VanillaBlocks;
use pocketmine\data\bedrock\block\BlockStateData;
use pocketmine\data\bedrock\block\BlockStateDeserializeException;
use pocketmine\data\bedrock\block\convert\BlockStateToObjectDeserializer;
use pocketmine\data\bedrock\block\upgrade\BlockStateUpgrader;
use pocketmine\nbt\InvalidTagValueException;
use pocketmine\nbt\tag\ByteTag;
use pocketmine\nbt\tag\IntTag;
use pocketmine\nbt\tag\StringTag;
use pocketmine\nbt\tag\Tag;
use pocketmine\utils\AssumptionFailedError;
use pocketmine\world\format\io\GlobalBlockStateHandlers;
use function count;

/**
 * Java block state id -> PocketMine-MP's own internal block state id.
 *
 * Stratum's table stops at the Bedrock blockstate {name, states, version}
 * (SPEC section 9); turning that into a state id is PocketMine-MP's own job,
 * through the very code its world loader uses for every palette entry on
 * disk. Doing it that way rather than with a second table of our own is the
 * point: PocketMine-MP stays the authority on what its block ids mean.
 *
 * One instance per generation worker thread. Every handler it touches is a
 * per-thread static, and the cache is a plain array — workers share nothing,
 * so there is nothing to lock.
 *
 * A state PocketMine-MP does not implement is NOT a crash and NOT a silent
 * substitution (SPEC section 9): it resolves through the fallback table
 * below and is logged once, naming the block.
 */
final class BlockStateTranslator{

	/**
	 * Blocks vanilla generates that PocketMine-MP has no block for, and what
	 * to put there instead. Measured, not guessed: PocketMine-MP 5.44.4 has
	 * no powder snow block at all (it appears only as a cauldron liquid),
	 * and the overworld's surface rules place it on snowy slopes and peaks.
	 *
	 * @return array<string, int> Bedrock state name => PocketMine-MP state id
	 */
	private static function fallbacks() : array{
		return [
			"minecraft:powder_snow" => VanillaBlocks::SNOW()->getStateId()
		];
	}

	/** @var array<int, int> Java block state id => PocketMine-MP state id */
	private array $cache = [];

	/** @var array<int, true> Java state ids already reported, so a miss is logged once, not once per block */
	private array $reported = [];

	private BlockStateToObjectDeserializer $deserializer;

	private BlockStateUpgrader $upgrader;

	/** @var array<string, int> */
	private array $fallbacks;

	/** PocketMine-MP's own "I do not know this block": minecraft:info_update. */
	private int $unknownStateId;

	public function __construct(){
		$this->deserializer = GlobalBlockStateHandlers::getDeserializer();
		// BlockDataUpgrader is the outer one; the method that takes a
		// BlockStateData is on the inner BlockStateUpgrader.
		$this->upgrader = GlobalBlockStateHandlers::getUpgrader()->getBlockStateUpgrader();
		$this->unknownStateId = $this->deserializer->deserialize(GlobalBlockStateHandlers::getUnknownBlockStateData());
		$this->fallbacks = self::fallbacks();
	}

	public function translate(int $javaStateId) : int{
		return $this->cache[$javaStateId] ??= $this->resolve($javaStateId);
	}

	/** How many distinct states this worker has translated, for diagnostics. */
	public function translatedCount() : int{
		return count($this->cache);
	}

	private function resolve(int $javaStateId) : int{
		/** @var array{name: string, states: array<string, array{int, int|string}>, version: int} $bedrock */
		$bedrock = \Stratum\bedrockBlockState($javaStateId);

		try{
			$states = [];
			foreach($bedrock["states"] as $name => [$tagType, $value]){
				$states[$name] = self::tag($tagType, $value);
			}
			// The explicit version is kept rather than restamped with
			// BlockStateData::current(): it is what Stratum's table was
			// generated at, and the upgrader is what moves it forward.
			$upgraded = $this->upgrader->upgrade(new BlockStateData($bedrock["name"], $states, $bedrock["version"]));

			return $this->deserializer->deserialize($upgraded);
		}catch(BlockStateDeserializeException | InvalidTagValueException | AssumptionFailedError $e){
			// UnsupportedBlockStateException (a BlockStateDeserializeException)
			// is the ordinary case here: PocketMine-MP does not implement the
			// block. The others mean the state did not survive PocketMine-MP's
			// own upgrade path. Either way the world still gets a block, and
			// the log says which one and why.
			return $this->fallbackFor($javaStateId, $bedrock["name"], $e->getMessage());
		}
	}

	private function fallbackFor(int $javaStateId, string $bedrockName, string $why) : int{
		$fallback = $this->fallbacks[$bedrockName] ?? $this->unknownStateId;
		if(!isset($this->reported[$javaStateId])){
			$this->reported[$javaStateId] = true;
			\GlobalLogger::get()->warning(
				"Stratum: PocketMine-MP cannot place " . $bedrockName . " (" . $why . "); using " .
				($fallback === $this->unknownStateId ? "minecraft:info_update" : "this build's configured fallback") .
				" for it in generated terrain"
			);
		}
		return $fallback;
	}

	private static function tag(int $nbtTagType, int|string $value) : Tag{
		// The tag ids Stratum's table carries, which are NBT's own.
		return match($nbtTagType){
			1 => new ByteTag((int) $value),
			3 => new IntTag((int) $value),
			8 => new StringTag((string) $value),
			default => throw new AssumptionFailedError("Stratum gave NBT tag type " . $nbtTagType . ", which a Bedrock blockstate cannot hold")
		};
	}
}
