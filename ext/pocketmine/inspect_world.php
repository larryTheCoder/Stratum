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
 * 8, round the level.dat spawn) is there and populated.
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
 * caught rather than repeated. Biomes must match exactly. A block may
 * differ only as PocketMine-MP's own liquid rules change terrain once the
 * plugin's FluidUpdateScheduler sets the engine's aquifer water and lava
 * flowing (a block update at each, when its chunk is populated):
 *
 *   air, now water or lava         the flow itself (Liquid::onScheduledUpdate)
 *   air, now stone                 flowing lava into flowed water (Lava::flowIntoBlock)
 *   air, now cobblestone           flowed lava hardened beside water (Lava::checkForHarden)
 *   water, now stone               flowing lava into the engine's water (Lava::flowIntoBlock)
 *   lava, now obsidian/cobblestone the engine's lava hardened beside water (Lava::checkForHarden)
 *
 * Each is counted and reported. Every other difference is a failure,
 * listed by kind with where it first occurs. How much flowed varies from
 * run to run — the server populates chunks on three workers, in whatever
 * order they finish — so the counts are reported, not pinned.
 *
 * Fails on a wrong generator, options or seed, on no chunks at all, on a
 * spawn-area chunk missing or unpopulated, and on any chunk that is not
 * what the engine generates for it, as above.
 */

namespace Stratum\PocketMineEvidence;

use pocketmine\block\Block;
use pocketmine\block\Lava;
use pocketmine\block\RuntimeBlockStateRegistry;
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

/** @var array<string, array{int, string}> $differences kind => [count, first "chunk x,z block x,y,z"] */
$differences = [];
$difference = static function(string $kind, string $where) use (&$differences) : void{
	if(isset($differences[$kind])){
		$differences[$kind][0]++;
	}else{
		$differences[$kind] = [1, $where];
	}
};
/** @var array<string, int> $flowed what the liquid rules made of the engine's terrain => how many blocks */
$flowed = [];
$blocksCompared = 0;
$columnsCompared = 0;

$stone = VanillaBlocks::STONE()->getStateId();
$cobblestone = VanillaBlocks::COBBLESTONE()->getStateId();
$obsidian = VanillaBlocks::OBSIDIAN()->getStateId();

/**
 * The change, if PocketMine-MP's own liquid rules make it from what the
 * engine generated ($want) to what is on disk ($got) — the table in this
 * file's header — or null.
 */
$fluidChange = static function(int $want, int $got) use ($registry, $stone, $cobblestone, $obsidian) : ?string{
	$now = $registry->fromStateId($got);
	if($want === Block::EMPTY_STATE_ID){
		return match(true){
			$now instanceof Water => "air, now water (flowed)",
			$now instanceof Lava => "air, now lava (flowed)",
			$got === $stone => "air, now stone (flowing lava into flowed water)",
			$got === $cobblestone => "air, now cobblestone (flowed lava hardened beside water)",
			default => null
		};
	}
	$was = $registry->fromStateId($want);
	if($was instanceof Water && $got === $stone){
		return "water, now stone (flowing lava into it)";
	}
	if($was instanceof Lava && ($got === $obsidian || $got === $cobblestone)){
		return "lava, now " . ($got === $obsidian ? "obsidian" : "cobblestone") . " (hardened beside water)";
	}
	return null;
};

/**
 * Every block of one sub-chunk on disk against the engine's: identical, a
 * change the liquid rules make (counted in $flowed), or a difference.
 */
$compareBlocks = static function(SubChunk $actual, ?PalettedBlockArray $expected, int $chunkX, int $chunkZ, int $subChunkY) use ($fluidChange, $name, $difference, &$flowed) : void{
	$layers = $actual->getBlockLayers();
	if(count($layers) > 1){
		$difference("a second block layer, which nothing generated", "chunk " . $chunkX . "," . $chunkZ . " sub-chunk " . $subChunkY);
	}
	$layer = $layers[0] ?? null;
	if($expected === null && $layer === null){
		return;
	}
	if($expected !== null && $layer !== null && $expected->getBitsPerBlock() === $layer->getBitsPerBlock() &&
		$expected->getPalette() === $layer->getPalette() && $expected->getWordArray() === $layer->getWordArray()){
		return;
	}
	for($x = 0; $x < 16; ++$x){
		for($z = 0; $z < 16; ++$z){
			for($y = 0; $y < 16; ++$y){
				$want = $expected?->get($x, $y, $z) ?? Block::EMPTY_STATE_ID;
				$got = $layer?->get($x, $y, $z) ?? Block::EMPTY_STATE_ID;
				if($want === $got){
					continue;
				}
				$change = $fluidChange($want, $got);
				if($change !== null){
					$flowed[$change] = ($flowed[$change] ?? 0) + 1;
					continue;
				}
				$difference($name($want) . " generated, " . $name($got) . " on disk",
					"chunk " . $chunkX . "," . $chunkZ . " block " . ($chunkX * 16 + $x) . "," . ($subChunkY * 16 + $y) . "," . ($chunkZ * 16 + $z));
			}
		}
	}
};

$logger = new CapturingLogger();
$provider = new LevelDB($world, $logger);
$onDisk = $provider->calculateChunkCount();

$populated = [];
$loaded = 0;
$blocks = [];
$biomes = [];
$withUnknown = 0;
foreach($provider->getAllChunks() as $coordinates => $chunk){
	[$chunkX, $chunkZ] = $coordinates;
	$loaded++;
	$data = $chunk->getData();
	if($data->isPopulated()){
		$populated[World::chunkHash($chunkX, $chunkZ)] = true;
	}
	$subChunks = $data->getSubChunks();
	$encoded = $dimension->encodeChunk($chunkX, $chunkZ);
	if(array_keys($encoded) !== $subChunkIndices){
		$failures[] = "encodeChunk(" . $chunkX . ", " . $chunkZ . ") gave sub-chunks " . implode(",", array_keys($encoded)) .
			", not " . Chunk::MIN_SUBCHUNK_INDEX . ".." . Chunk::MAX_SUBCHUNK_INDEX;
		continue;
	}
	$hasUnknown = false;
	foreach($subChunkIndices as $subChunkY){
		$subChunk = $subChunks[$subChunkY] ?? null;
		if($subChunk === null){
			$difference("a sub-chunk missing on disk", "chunk " . $chunkX . "," . $chunkZ . " sub-chunk " . $subChunkY);
			continue;
		}
		foreach($subChunk->getBlockLayers() as $layer){
			foreach($layer->getPalette() as $stateId){
				$hasUnknown = $hasUnknown || $stateId === $unknownId;
				$blocks[$name($stateId)] = ($blocks[$name($stateId)] ?? 0) + 1;
			}
		}
		foreach($subChunk->getBiomeArray()->getPalette() as $biomeId){
			$biomes[$biomeId] = true;
		}

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
		$compareBlocks($subChunk, $expected, $chunkX, $chunkZ, $subChunkY);
		$blocksCompared += 4096;

		[$biomeBits, $biomeWords, $biomePalette] = $layers["biomes"];
		$expectedBiomes = PalettedBlockArray::fromData($biomeBits, $biomeWords, $biomePalette);
		$expectedBiomes->collectGarbage();
		$actualBiomes = $subChunk->getBiomeArray();
		if($expectedBiomes->getBitsPerBlock() !== $actualBiomes->getBitsPerBlock() ||
			$expectedBiomes->getPalette() !== $actualBiomes->getPalette() || $expectedBiomes->getWordArray() !== $actualBiomes->getWordArray()){
			for($x = 0; $x < 16; ++$x){
				for($z = 0; $z < 16; ++$z){
					for($y = 0; $y < 16; ++$y){
						$want = $expectedBiomes->get($x, $y, $z);
						$got = $actualBiomes->get($x, $y, $z);
						if($want !== $got){
							$difference("biome " . $want . " generated, " . $got . " on disk",
								"chunk " . $chunkX . "," . $chunkZ . " block " . ($chunkX * 16 + $x) . "," . ($subChunkY * 16 + $y) . "," . ($chunkZ * 16 + $z));
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

$spawnChunks = 0;
$missing = [];
foreach((new ChunkSelector())->selectChunks(8, $spawnX >> 4, $spawnZ >> 4) as $hash){
	$spawnChunks++;
	if(!isset($populated[$hash])){
		World::getXZ($hash, $x, $z);
		$missing[] = $x . "," . $z;
	}
}

echo "chunks on disk: ", $onDisk, " (", $loaded, " loaded by PocketMine-MP's LevelDB provider), ", count($populated), " populated\n";
echo "spawn area (ChunkSelector radius 8 round the spawn): ", $spawnChunks, " chunks, ", $spawnChunks - count($missing), " on disk and populated\n";
arsort($blocks);
echo "Bedrock blocks present (sub-chunk palettes holding each): ";
$parts = [];
foreach($blocks as $blockName => $count){
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
ksort($flowed);
echo "  changed by PocketMine-MP's liquid rules since the aquifer fluid was set flowing: ", array_sum($flowed), " blocks", count($flowed) > 0 ? ":" : "", "\n";
foreach($flowed as $change => $count){
	echo "    ", $change, " x", $count, "\n";
}
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
