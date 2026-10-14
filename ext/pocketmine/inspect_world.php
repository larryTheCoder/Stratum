<?php

declare(strict_types=1);

/*
 * Stratum — what a stopped PocketMine-MP server left on disk for a world,
 * read with PocketMine-MP 5.44.3's own classes, against what Stratum's
 * engine generates for it (tools/pmmp-dev smoke).
 *
 *   php -d extension=stratum.so inspect_world.php --phar=PocketMine-MP.phar
 *       --world=<server>/worlds/<name> --generator=stratum --options=<json>
 *       --seed=<seed>
 *
 * level.dat is read raw — its little-endian NBT after the 8-byte header,
 * with none of BedrockWorldData's fix-ups — so the generator name, options
 * and seed are what the server wrote. Chunks are read through
 * PocketMine-MP's own LevelDB provider, as the server reads them: every
 * chunk on disk, which are populated, and whether every chunk of the spawn
 * area WorldManager::generateWorld() asks to populate (ChunkSelector, radius
 * 8, round the level.dat spawn) is there and populated. That spawn area must
 * hold some terrain in what the engine generates: a comparison of air with
 * air would pass a generator that drops every block.
 *
 * Then what every chunk on disk HOLDS, populated or not, block by block and
 * biome by biome, against what it should: the world's own options — the
 * plugin's GeneratorOptions::parse, the frozen blob in the world's folder,
 * the noise settings and biome source — opened as a Stratum\Dimension at
 * the level.dat seed, each chunk encoded, and each Java block state put
 * through the plugin's own BlockStateTranslator (PocketMine-MP's upgrader
 * and deserializer). The comparison reads the encoded sub-chunks by their
 * own indices, not through the plugin's StratumGenerator, so a generator
 * that assembles a chunk wrongly — shifted, emptied, mistranslated — is
 * caught rather than repeated. Biomes must match exactly.
 *
 * A block may differ only as PocketMine-MP 5.44.3's own rules change terrain
 * once the plugin's FluidUpdateScheduler sets the engine's aquifer water and
 * lava flowing — and only where those rules allow it, judged from the blocks
 * round it on disk and in what the engine generated (Liquid, Water, Lava and
 * FallableTrait, read from 5.44.3's source):
 *
 *   air, or a block liquid flows into, now water or lava (flowIntoBlock,
 *   onScheduledUpdate):
 *     falling            the block above it on disk is the same liquid
 *     flowing (decay d)  a horizontal neighbour on disk is the same liquid
 *                        with a lower decay (a falling one counts as 0)
 *     a source           water only (Water::getMinAdjacentSourcesToFormSource
 *                        is 2; lava has none): at least two horizontal water
 *                        sources beside it on disk and something solid or a
 *                        water source under it — and the sources it formed
 *                        from must lead back to the engine's own: every
 *                        formed source is reached by adding, one at a time,
 *                        a formed source with two horizontal neighbours
 *                        among the engine's water sources and those already
 *                        added
 *   air or the engine's water, now stone
 *                      lava flowed into water (Lava::flowIntoBlock): lava,
 *                      or lava's hardened remains (obsidian, cobblestone),
 *                      above it or beside it
 *   air, now cobblestone; the engine's lava, now obsidian (a source) or
 *   cobblestone (decay 1..4)
 *                      lava hardened beside water (Lava::checkForHarden):
 *                      water, or water turned stone, above it or beside it
 *   a gravel, sand or snow block (Fallable), now air or a liquid
 *                      it fell (FallableTrait::onNearbyBlockChange): what
 *                      the engine put under it can be replaced (air, a
 *                      liquid), or fell itself; one it left as a liquid
 *                      holds to the liquid rules above as well
 *   air or a liquid, now a Fallable
 *                      where one landed (FallingBlock): one of the same
 *                      state fell from higher in the same column, one
 *                      landing for each that fell
 *
 * Each is counted and reported by kind. Every other difference — and every
 * one of those kinds where its rule does not hold — is a failure, listed by
 * kind with where it first occurs. And the whole is bounded by the engine's
 * own liquid: no more blocks changed than MAX_CHANGES_PER_MARKED for each
 * fluid position the engine marked for an update in the populated chunks —
 * the positions Stratum\takeFluidUpdates hands the plugin's
 * FluidUpdateScheduler as each is populated, where every flow starts — so a
 * run that flooded the world cannot pass however locally plausible each
 * block is. How
 * much flowed varies from run to run — the server populates chunks on three
 * workers, in whatever order they finish — so the counts are reported, not
 * pinned.
 *
 * Fails on a wrong generator, options or seed, on no chunks at all, on a
 * spawn-area chunk missing or unpopulated, on a spawn area the engine fills
 * with nothing but air, and on any chunk that is not what the engine
 * generates for it, as above.
 */

namespace Stratum\PocketMineEvidence;

use pocketmine\block\Block;
use pocketmine\block\Lava;
use pocketmine\block\Liquid;
use pocketmine\block\RuntimeBlockStateRegistry;
use pocketmine\block\utils\Fallable;
use pocketmine\block\VanillaBlocks;
use pocketmine\block\Water;
use pocketmine\nbt\LittleEndianNbtSerializer;
use pocketmine\player\ChunkSelector;
use pocketmine\world\format\Chunk;
use pocketmine\world\format\PalettedBlockArray;
use pocketmine\world\format\SubChunk;
use pocketmine\world\format\io\GlobalBlockStateHandlers;
use pocketmine\world\format\io\leveldb\LevelDB;
use pocketmine\world\World;
use Stratum\PMMP\BlockStateTranslator;
use Stratum\PMMP\GeneratorOptions;

require __DIR__ . "/bootstrap.php";

/**
 * How many blocks PocketMine-MP's rules may change for each fluid position
 * the engine marked in the populated chunks. Measured, smoke and observe,
 * over five seeds: from 1 change in 50 marked positions (8675309) to 1 in 2
 * (-4242, a waterfall into caves); most marked positions are the surface of
 * still water, which goes nowhere. Four is that worst case eight times over.
 */
const MAX_CHANGES_PER_MARKED = 4;

$args = arguments($argv, ["phar", "world", "generator", "options", "seed"]);
bootstrap($args["phar"]);
$world = $args["world"];
$failures = [];

$raw = (string) @file_get_contents($world . "/level.dat");
if(strlen($raw) <= 8){
	fail("no level.dat in " . $world);
}
$levelDat = (new LittleEndianNbtSerializer())->read(substr($raw, 8))->mustGetCompoundTag();
$generator = $levelDat->getString("generatorName", "(none)");
$options = $levelDat->getString("generatorOptions", "(none)");
$seed = $levelDat->getLong("RandomSeed", 0);
$spawnX = $levelDat->getInt("SpawnX", 0);
$spawnZ = $levelDat->getInt("SpawnZ", 0);
echo "level.dat generatorName: ", $generator, "\n";
echo "level.dat generatorOptions: ", $options, "\n";
echo "level.dat RandomSeed: ", $seed, ", spawn ", $spawnX, " ", $levelDat->getInt("SpawnY", 0), " ", $spawnZ, "\n";
if($generator !== $args["generator"]){
	$failures[] = "level.dat names generator " . $generator . ", not " . $args["generator"];
}
if($options !== $args["options"]){
	$failures[] = "level.dat holds generator options " . $options . ", not " . $args["options"];
}
if($seed !== (int) $args["seed"]){
	$failures[] = "level.dat holds seed " . $seed . ", not " . $args["seed"];
}

// What the engine generates for this world: its options as the plugin reads
// them, its seed as level.dat holds it.
$parsed = GeneratorOptions::parse($options);
$dimension = \Stratum\Dimension::open($parsed->blobPath, $parsed->noiseSettings, $parsed->biomeSource, $seed);
$translatorLog = new CapturingLogger();
\GlobalLogger::set($translatorLog);
$translator = new BlockStateTranslator();
$registry = RuntimeBlockStateRegistry::getInstance();
$serializer = GlobalBlockStateHandlers::getSerializer();
$unknownId = GlobalBlockStateHandlers::getDeserializer()->deserialize(GlobalBlockStateHandlers::getUnknownBlockStateData());
$subChunkIndices = range(Chunk::MIN_SUBCHUNK_INDEX, Chunk::MAX_SUBCHUNK_INDEX);

/** @var array<int, string> $names state id => Bedrock blockstate name */
$names = [];
$name = static function(int $stateId) use (&$names, $serializer) : string{
	return $names[$stateId] ??= $serializer->serialize($stateId)->getName();
};
/** @var array<int, Block> $blocks state id => the block, never positioned or changed */
$blockCache = [];
$block = static function(int $stateId) use (&$blockCache, $registry) : Block{
	return $blockCache[$stateId] ??= $registry->fromStateId($stateId);
};

/** @var array<string, array{int, string}> $differences kind => [count, first "chunk x,z block x,y,z"] */
$differences = [];
$difference = static function(string $kind, string $where) use (&$differences) : void{
	if(isset($differences[$kind])){
		$differences[$kind][0]++;
	}else{
		$differences[$kind] = [1, $where];
	}
};
$at = static function(int $x, int $y, int $z) : string{
	return "chunk " . ($x >> 4) . "," . ($z >> 4) . " block " . $x . "," . $y . "," . $z;
};
$blocksCompared = 0;
$columnsCompared = 0;

/**
 * Every chunk on disk, as PocketMine-MP's provider read it, and what the
 * engine generates for it: chunk hash => sub-chunk index => SubChunk, and
 * => the expected block layer (null for all air).
 *
 * @var array<int, array<int, SubChunk>> $onDiskSubChunks
 * @var array<int, array<int, PalettedBlockArray|null>> $expectedLayers
 */
$onDiskSubChunks = [];
$expectedLayers = [];
/** @var array<int, true> $changed block hash => true: every block that differs from the engine's */
$changed = [];
$markedCount = 0;
/** The same, in the populated chunks alone: those the plugin was handed. */
$markedPopulated = 0;

/**
 * The block on disk at a world position, or null outside the chunks on disk.
 */
$got = static function(int $x, int $y, int $z) use (&$onDiskSubChunks) : ?int{
	if($y < World::Y_MIN || $y >= World::Y_MAX){
		return null;
	}
	$subChunks = $onDiskSubChunks[World::chunkHash($x >> 4, $z >> 4)] ?? null;
	if($subChunks === null){
		return null;
	}
	return ($subChunks[$y >> 4] ?? null)?->getBlockStateId($x & 0xf, $y & 0xf, $z & 0xf) ?? Block::EMPTY_STATE_ID;
};
/**
 * What the engine generates at a world position, or null outside the chunks on disk.
 */
$want = static function(int $x, int $y, int $z) use (&$expectedLayers) : ?int{
	if($y < World::Y_MIN || $y >= World::Y_MAX){
		return null;
	}
	$layers = $expectedLayers[World::chunkHash($x >> 4, $z >> 4)] ?? null;
	if($layers === null){
		return null;
	}
	return ($layers[$y >> 4] ?? null)?->get($x & 0xf, $y & 0xf, $z & 0xf) ?? Block::EMPTY_STATE_ID;
};

$logger = new CapturingLogger();
$provider = new LevelDB($world, $logger);
$onDisk = $provider->calculateChunkCount();

$populated = [];
$loaded = 0;
$presentBlocks = [];
$biomes = [];
$withUnknown = 0;
foreach($provider->getAllChunks() as $coordinates => $chunk){
	[$chunkX, $chunkZ] = $coordinates;
	$hash = World::chunkHash($chunkX, $chunkZ);
	$loaded++;
	$data = $chunk->getData();
	if($data->isPopulated()){
		$populated[$hash] = true;
	}
	$subChunks = $data->getSubChunks();
	$encoded = $dimension->encodeChunk($chunkX, $chunkZ);
	// What encodeChunk left for the plugin: the positions a server's
	// FluidUpdateScheduler sets flowing when this chunk is populated.
	$updates = \Stratum\takeFluidUpdates($parsed->blobPath, $parsed->noiseSettings, $parsed->biomeSource, $seed, $chunkX, $chunkZ);
	$markedCount += count($updates);
	if(isset($populated[$hash])){
		$markedPopulated += count($updates);
	}
	if(array_keys($encoded) !== $subChunkIndices){
		$failures[] = "encodeChunk(" . $chunkX . ", " . $chunkZ . ") gave sub-chunks " . implode(",", array_keys($encoded)) .
			", not " . Chunk::MIN_SUBCHUNK_INDEX . ".." . Chunk::MAX_SUBCHUNK_INDEX;
		continue;
	}
	$onDiskSubChunks[$hash] = $subChunks;
	$expectedLayers[$hash] = [];
	$hasUnknown = false;
	foreach($subChunkIndices as $subChunkY){
		$layers = $encoded[$subChunkY];
		$expected = null;
		if($layers["blocks"] !== null){
			[$bitsPerBlock, $wordArray, $javaPalette] = $layers["blocks"];
			$palette = [];
			foreach($javaPalette as $javaStateId){
				$palette[] = $translator->translate($javaStateId);
			}
			$expected = PalettedBlockArray::fromData($bitsPerBlock, $wordArray, $palette);
			// Compacted as SubChunk::collectGarbage() compacts what the
			// server saves — the same blocks, so an unchanged sub-chunk is
			// then the same words and palette, and the block-by-block walk
			// is left for the ones that differ. A layer that compacts to
			// nothing but air is dropped there; so here.
			$expected->collectGarbage();
			if($expected->getBitsPerBlock() === 0 && $expected->get(0, 0, 0) === Block::EMPTY_STATE_ID){
				$expected = null;
			}
		}
		$expectedLayers[$hash][$subChunkY] = $expected;

		$subChunk = $subChunks[$subChunkY] ?? null;
		if($subChunk === null){
			$difference("a sub-chunk missing on disk", "chunk " . $chunkX . "," . $chunkZ . " sub-chunk " . $subChunkY);
			continue;
		}
		foreach($subChunk->getBlockLayers() as $layer){
			foreach($layer->getPalette() as $stateId){
				$hasUnknown = $hasUnknown || $stateId === $unknownId;
				$presentBlocks[$name($stateId)] = ($presentBlocks[$name($stateId)] ?? 0) + 1;
			}
		}
		foreach($subChunk->getBiomeArray()->getPalette() as $biomeId){
			$biomes[$biomeId] = true;
		}

		// Blocks: identical, or every block that differs noted, to be
		// judged against PocketMine-MP's rules once every chunk is read.
		$layerList = $subChunk->getBlockLayers();
		if(count($layerList) > 1){
			$difference("a second block layer, which nothing generated", "chunk " . $chunkX . "," . $chunkZ . " sub-chunk " . $subChunkY);
		}
		$layer = $layerList[0] ?? null;
		$blocksCompared += 4096;
		if(!($expected === null && $layer === null) && !($expected !== null && $layer !== null &&
			$expected->getBitsPerBlock() === $layer->getBitsPerBlock() &&
			$expected->getPalette() === $layer->getPalette() && $expected->getWordArray() === $layer->getWordArray())){
			for($x = 0; $x < 16; ++$x){
				for($z = 0; $z < 16; ++$z){
					for($y = 0; $y < 16; ++$y){
						$wanted = $expected?->get($x, $y, $z) ?? Block::EMPTY_STATE_ID;
						$found = $layer?->get($x, $y, $z) ?? Block::EMPTY_STATE_ID;
						if($wanted !== $found){
							$changed[World::blockHash($chunkX * 16 + $x, $subChunkY * 16 + $y, $chunkZ * 16 + $z)] = true;
						}
					}
				}
			}
		}

		[$biomeBits, $biomeWords, $biomePalette] = $layers["biomes"];
		$expectedBiomes = PalettedBlockArray::fromData($biomeBits, $biomeWords, $biomePalette);
		$expectedBiomes->collectGarbage();
		$actualBiomes = $subChunk->getBiomeArray();
		if($expectedBiomes->getBitsPerBlock() !== $actualBiomes->getBitsPerBlock() ||
			$expectedBiomes->getPalette() !== $actualBiomes->getPalette() || $expectedBiomes->getWordArray() !== $actualBiomes->getWordArray()){
			for($x = 0; $x < 16; ++$x){
				for($z = 0; $z < 16; ++$z){
					for($y = 0; $y < 16; ++$y){
						$wantBiome = $expectedBiomes->get($x, $y, $z);
						$gotBiome = $actualBiomes->get($x, $y, $z);
						if($wantBiome !== $gotBiome){
							$difference("biome " . $wantBiome . " generated, " . $gotBiome . " on disk",
								$at($chunkX * 16 + $x, $subChunkY * 16 + $y, $chunkZ * 16 + $z));
						}
					}
				}
			}
		}
	}
	$columnsCompared++;
	if($hasUnknown){
		$withUnknown++;
	}
}
$provider->close();

// --- every changed block, judged by PocketMine-MP's own rules ----------------

$stone = VanillaBlocks::STONE()->getStateId();
$cobblestone = VanillaBlocks::COBBLESTONE()->getStateId();
$obsidian = VanillaBlocks::OBSIDIAN()->getStateId();
$horizontal = [[1, 0, 0], [-1, 0, 0], [0, 0, 1], [0, 0, -1]];
$exceptDown = [[0, 1, 0], ...$horizontal];

/** The effective decay PocketMine-MP flows by (Liquid::getEffectiveFlowDecay): a falling liquid counts as 0. */
$effectiveDecay = static fn(Liquid $liquid) : int => $liquid->isFalling() ? 0 : $liquid->getDecay();

/**
 * Whether some neighbour at one of $offsets from (x, y, z) passes $test,
 * given what is on disk there and what the engine generated there.
 *
 * @param list<array{int, int, int}> $offsets
 * @param \Closure(?Block, ?Block) : bool $test (on disk, generated)
 */
$anyNeighbour = static function(int $x, int $y, int $z, array $offsets, \Closure $test) use ($got, $want, $block) : bool{
	foreach($offsets as [$dx, $dy, $dz]){
		$onDisk = $got($x + $dx, $y + $dy, $z + $dz);
		$generated = $want($x + $dx, $y + $dy, $z + $dz);
		if($test($onDisk === null ? null : $block($onDisk), $generated === null ? null : $block($generated))){
			return true;
		}
	}
	return false;
};
/**
 * Lava at a neighbour, on disk or as the engine generated it, or lava's
 * hardened remains (obsidian or cobblestone where the engine put lava or
 * air): what can have flowed into water there.
 */
$lavaSide = static fn(?Block $onDisk, ?Block $generated) : bool =>
	$onDisk instanceof Lava || $generated instanceof Lava ||
	(($onDisk?->getStateId() === $cobblestone || $onDisk?->getStateId() === $obsidian) && $generated?->getStateId() === Block::EMPTY_STATE_ID);
/**
 * Water at a neighbour, on disk or as the engine generated it, or water that
 * lava turned to stone (stone where the engine put water or air): what can
 * have hardened lava there.
 */
$waterSide = static fn(?Block $onDisk, ?Block $generated) : bool =>
	$onDisk instanceof Water || $generated instanceof Water ||
	($onDisk?->getStateId() === $stone && $generated?->getStateId() === Block::EMPTY_STATE_ID);

/**
 * Why a liquid on disk where the engine put none breaks the liquid rules, or
 * null if it keeps them. A source it formed is noted in $formed, for the
 * check that every one leads back to the engine's own.
 *
 * @param array<int, true> $formed
 */
$flowedLiquid = static function(int $x, int $y, int $z, Liquid $liquid, array &$formed) use ($got, $block, $horizontal, $effectiveDecay) : ?string{
	$kind = $liquid instanceof Water ? "water" : "lava";
	if($liquid->isFalling()){
		$above = $got($x, $y + 1, $z);
		$above = $above === null ? null : $block($above);
		return $above instanceof Liquid && $above->hasSameTypeId($liquid) ? null : "falling " . $kind . " with no " . $kind . " above it";
	}
	if($liquid->getDecay() > 0){
		foreach($horizontal as [$dx, , $dz]){
			$side = $got($x + $dx, $y, $z + $dz);
			$side = $side === null ? null : $block($side);
			if($side instanceof Liquid && $side->hasSameTypeId($liquid) && $effectiveDecay($side) < $liquid->getDecay()){
				return null;
			}
		}
		return "flowing " . $kind . " with no " . $kind . " beside it that it can have flowed from";
	}
	$minimum = $liquid->getMinAdjacentSourcesToFormSource();
	if($minimum === null){
		return "a " . $kind . " source, which PocketMine-MP never forms";
	}
	$sources = 0;
	foreach($horizontal as [$dx, , $dz]){
		$side = $got($x + $dx, $y, $z + $dz);
		$side = $side === null ? null : $block($side);
		if($side instanceof Liquid && $side->hasSameTypeId($liquid) && $side->isSource()){
			$sources++;
		}
	}
	if($sources < $minimum){
		return "a " . $kind . " source with " . $sources . " " . $kind . " source(s) beside it, not " . $minimum;
	}
	$below = $got($x, $y - 1, $z);
	$below = $below === null ? null : $block($below);
	if(!($below !== null && ($below->isSolid() || ($below instanceof Liquid && $below->hasSameTypeId($liquid) && $below->isSource())))){
		return "a " . $kind . " source with nothing solid and no source under it";
	}
	$formed[World::blockHash($x, $y, $z)] = true;
	return null;
};

/** @var array<string, int> $flowed kind => how many blocks PocketMine-MP's rules changed so */
$flowed = [];
$allowed = static function(string $kind) use (&$flowed) : void{
	$flowed[$kind] = ($flowed[$kind] ?? 0) + 1;
};
/** @var array<int, true> $formed water sources formed on disk, to be led back to the engine's */
$formed = [];
/** @var array<int, string> $pendingSources block hash => its kind: counted once it is led back to the engine's sources */
$pendingSources = [];
$allowedOrFormed = static function(int $hash, string $kind) use (&$formed, &$pendingSources, $allowed) : void{
	if(isset($formed[$hash])){
		$pendingSources[$hash] = $kind;
	}else{
		$allowed($kind);
	}
};
/** @var array<string, array<int, list<int>>> $fell "x,z" => state id => the y of each Fallable that fell from there */
$fell = [];
/** @var array<string, array<int, list<int>>> $landed "x,z" => state id => the y of each that landed there */
$landed = [];

foreach($changed as $hash => $_){
	World::getBlockXYZ($hash, $x, $y, $z);
	$wantId = $want($x, $y, $z) ?? Block::EMPTY_STATE_ID;
	$gotId = $got($x, $y, $z) ?? Block::EMPTY_STATE_ID;
	$was = $block($wantId);
	$now = $block($gotId);
	$where = $at($x, $y, $z);

	// Liquid flowing into air, or into a block it breaks (a snow layer).
	if($now instanceof Liquid && !($was instanceof Liquid) && $was->canBeFlowedInto()){
		$why = $flowedLiquid($x, $y, $z, $now, $formed);
		if($why !== null){
			$difference($name($wantId) . " generated, " . $name($gotId) . " on disk: " . $why, $where);
			continue;
		}
		$state = $now->isFalling() ? "falling" : ($now->isSource() ? "a source" : "flowing");
		$allowedOrFormed($hash, $name($wantId) . ", now " . $name($gotId) . " (" . $state . ")");
		continue;
	}
	// A Fallable that fell: what was under it could be replaced, or fell too.
	if($was instanceof Fallable && ($gotId === Block::EMPTY_STATE_ID || $now instanceof Liquid)){
		$underId = $want($x, $y - 1, $z);
		$under = $underId === null ? null : $block($underId);
		$underFell = isset($changed[World::blockHash($x, $y - 1, $z)]) && $under instanceof Fallable;
		if(!($under !== null && ($under->canBeReplaced() || $underFell))){
			$difference($name($wantId) . " generated, " . $name($gotId) . " on disk, with nothing under it to fall into", $where);
			continue;
		}
		$fell[$x . "," . $z][$wantId][] = $y;
		if($now instanceof Liquid){
			$why = $flowedLiquid($x, $y, $z, $now, $formed);
			if($why !== null){
				$difference($name($wantId) . " generated, " . $name($gotId) . " on disk: " . $why, $where);
				continue;
			}
			$allowedOrFormed($hash, $name($wantId) . ", fell, now " . $name($gotId) . " (flowed in)");
		}else{
			$allowed($name($wantId) . ", now air (fell)");
		}
		continue;
	}
	// Where a Fallable landed: checked against those that fell, below.
	if($now instanceof Fallable && ($wantId === Block::EMPTY_STATE_ID || $was instanceof Liquid)){
		$landed[$x . "," . $z][$gotId][] = $y;
		continue;
	}
	// Lava flowing into water: the water turns to stone.
	if($gotId === $stone && ($wantId === Block::EMPTY_STATE_ID || $was instanceof Water)){
		if(!$anyNeighbour($x, $y, $z, $exceptDown, $lavaSide)){
			$difference($name($wantId) . " generated, stone on disk, with no lava above or beside it to have flowed into it", $where);
			continue;
		}
		$allowed($name($wantId) . ", now stone (lava flowed into water)");
		continue;
	}
	// Lava hardening beside water.
	$hardened = match(true){
		$wantId === Block::EMPTY_STATE_ID && $gotId === $cobblestone => true, // lava that flowed there, decay 1..4
		$was instanceof Lava && !$was->isFalling() && $was->isSource() && $gotId === $obsidian => true,
		$was instanceof Lava && !$was->isFalling() && $was->getDecay() >= 1 && $was->getDecay() <= 4 && $gotId === $cobblestone => true,
		default => false
	};
	if($hardened){
		if(!$anyNeighbour($x, $y, $z, $exceptDown, $waterSide)){
			$difference($name($wantId) . " generated, " . $name($gotId) . " on disk, with no water above or beside it to have hardened lava", $where);
			continue;
		}
		$allowed($name($wantId) . ", now " . $name($gotId) . " (lava hardened beside water)");
		continue;
	}
	$difference($name($wantId) . " generated, " . $name($gotId) . " on disk", $where);
}

// Every formed water source leads back to the engine's own: add, one at a
// time, a formed source with two horizontal neighbours among the engine's
// water sources and those already added. What is never added could not
// have formed.
$isEngineSource = static function(int $x, int $y, int $z) use ($want, $block) : bool{
	$generated = $want($x, $y, $z);
	if($generated === null){
		return false;
	}
	$was = $block($generated);
	return $was instanceof Water && $was->isSource();
};
$joined = [];
$queue = array_keys($formed);
while(count($queue) > 0){
	$next = [];
	foreach($queue as $hash){
		if(isset($joined[$hash])){
			continue;
		}
		World::getBlockXYZ($hash, $x, $y, $z);
		$support = 0;
		foreach($horizontal as [$dx, , $dz]){
			$side = World::blockHash($x + $dx, $y, $z + $dz);
			if(isset($joined[$side]) || (!isset($formed[$side]) && $isEngineSource($x + $dx, $y, $z + $dz))){
				$support++;
			}
		}
		if($support >= 2){
			$joined[$hash] = true;
			foreach($horizontal as [$dx, , $dz]){
				$side = World::blockHash($x + $dx, $y, $z + $dz);
				if(isset($formed[$side]) && !isset($joined[$side])){
					$next[] = $side;
				}
			}
		}
	}
	$queue = $next;
}
foreach($pendingSources as $hash => $kind){
	if(isset($joined[$hash])){
		$allowed($kind);
	}else{
		World::getBlockXYZ($hash, $x, $y, $z);
		$difference($kind . ": a water source no chain of sources leads to from the engine's", $at($x, $y, $z));
	}
}

// Every Fallable that landed fell from higher in its own column, one each.
foreach($landed as $column => $byState){
	[$x, $z] = array_map(intval(...), explode(",", $column));
	foreach($byState as $stateId => $landings){
		$sources = $fell[$column][$stateId] ?? [];
		rsort($sources);
		rsort($landings);
		foreach($landings as $landingY){
			// the highest that fell from above it and is not yet spent
			$match = null;
			foreach($sources as $i => $fallY){
				if($fallY > $landingY){
					$match = $i;
					break;
				}
			}
			$wantId = $want($x, $landingY, $z) ?? Block::EMPTY_STATE_ID;
			if($match === null){
				$difference($name($wantId) . " generated, " . $name($stateId) . " on disk, with no " . $name($stateId) . " that fell from above it", $at($x, $landingY, $z));
				continue;
			}
			unset($sources[$match]);
			$allowed($name($wantId) . ", now " . $name($stateId) . " (landed from above)");
		}
	}
}

$changes = array_sum($flowed);
$bound = MAX_CHANGES_PER_MARKED * $markedPopulated;
if($changes > $bound){
	$failures[] = $changes . " blocks changed by liquid and falling-block rules, more than " . MAX_CHANGES_PER_MARKED .
		" for each of the " . $markedPopulated . " fluid positions the engine marked for an update in the populated chunks, where every flow starts";
}

// --- the spawn area ---------------------------------------------------------

$spawnChunks = 0;
$missing = [];
$terrain = 0;
foreach((new ChunkSelector())->selectChunks(8, $spawnX >> 4, $spawnZ >> 4) as $hash){
	$spawnChunks++;
	if(!isset($populated[$hash])){
		World::getXZ($hash, $x, $z);
		$missing[] = $x . "," . $z;
	}
	foreach($expectedLayers[$hash] ?? [] as $expected){
		if($expected !== null){
			$terrain += 4096;
		}
	}
}

echo "chunks on disk: ", $onDisk, " (", $loaded, " loaded by PocketMine-MP's LevelDB provider), ", count($populated), " populated\n";
echo "spawn area (ChunkSelector radius 8 round the spawn): ", $spawnChunks, " chunks, ", $spawnChunks - count($missing), " on disk and populated; ",
	$terrain, " blocks of it in sub-chunks the engine fills with more than air\n";
arsort($presentBlocks);
echo "Bedrock blocks present (sub-chunk palettes holding each): ";
$parts = [];
foreach($presentBlocks as $blockName => $count){
	$parts[] = $blockName . " " . $count;
}
echo implode(", ", $parts), "\n";
ksort($biomes);
echo "biome ids present: ", implode(",", array_keys($biomes)), "\n";
echo "chunks holding minecraft:info_update: ", $withUnknown, "\n";
foreach($logger->records as [$level, $message]){
	echo "provider logged ", $level, ": ", $message, "\n";
}
echo "against the engine: ", $columnsCompared, " chunks encoded from the world's own options and seed, ", $blocksCompared, " blocks and as many biome entries compared\n";
echo "  fluid positions the engine marked for an update in these chunks: ", $markedCount, ", ", $markedPopulated, " of them in the populated ones\n";
ksort($flowed);
echo "  changed by PocketMine-MP's liquid and falling-block rules since the aquifer fluid was set flowing: ", $changes, " blocks",
	" (at most ", $bound, ")", count($flowed) > 0 ? ":" : "", "\n";
foreach($flowed as $change => $count){
	echo "    ", $change, " x", $count, "\n";
}
echo "  water sources formed beside the engine's: ", count($joined), " of ", count($formed), "\n";
foreach($translatorLog->records as [, $message]){
	echo "  translator: ", $message, "\n";
}
if(count($differences) === 0){
	echo "  no other difference, block or biome\n";
}
foreach($differences as $kind => [$count, $where]){
	echo "  DIFFERENT: ", $kind, " x", $count, ", first at ", $where, "\n";
}

if($onDisk === 0){
	$failures[] = "no chunks on disk";
}
if(count($missing) > 0){
	$failures[] = count($missing) . " spawn-area chunks missing or unpopulated: " . implode(" ", array_slice($missing, 0, 20));
}
if($terrain === 0 && $spawnChunks > 0){
	$failures[] = "the engine generates nothing but air in the spawn area round " . $spawnX . "," . $spawnZ .
		", so comparing it would pass a generator that drops every block";
}
if(count($differences) > 0){
	$failures[] = count($differences) . " kind(s) of difference between the chunks on disk and what the engine generates for them (listed above)";
}
if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
