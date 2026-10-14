<?php

declare(strict_types=1);

/*
 * Stratum — what a stopped PocketMine-MP server left on disk for a world,
 * read with PocketMine-MP 5.44.3's own classes (tools/pmmp-dev smoke).
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
 * 8, round the level.dat spawn) is there and populated. What the chunks
 * hold is summarised from their palettes: the Bedrock blocks and biome ids
 * present, and how many chunks hold minecraft:info_update, the block
 * PocketMine-MP's own deserializer and the plugin's translator use for one
 * they cannot place.
 *
 * Fails on a wrong generator, options or seed, on no chunks at all, and on
 * a spawn-area chunk missing or unpopulated.
 */

namespace Stratum\PocketMineEvidence;

use pocketmine\nbt\LittleEndianNbtSerializer;
use pocketmine\player\ChunkSelector;
use pocketmine\world\format\io\GlobalBlockStateHandlers;
use pocketmine\world\format\io\leveldb\LevelDB;
use pocketmine\world\World;

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

$logger = new CapturingLogger();
$provider = new LevelDB($world, $logger);
$onDisk = $provider->calculateChunkCount();
$serializer = GlobalBlockStateHandlers::getSerializer();
$unknownId = GlobalBlockStateHandlers::getDeserializer()->deserialize(GlobalBlockStateHandlers::getUnknownBlockStateData());

$populated = [];
$loaded = 0;
$blocks = [];
$biomes = [];
$withUnknown = 0;
$names = [];
foreach($provider->getAllChunks() as $coordinates => $chunk){
	[$chunkX, $chunkZ] = $coordinates;
	$loaded++;
	$data = $chunk->getData();
	if($data->isPopulated()){
		$populated[World::chunkHash($chunkX, $chunkZ)] = true;
	}
	$hasUnknown = false;
	foreach($data->getSubChunks() as $subChunk){
		foreach($subChunk->getBlockLayers() as $layer){
			foreach($layer->getPalette() as $stateId){
				$hasUnknown = $hasUnknown || $stateId === $unknownId;
				$name = $names[$stateId] ??= $serializer->serialize($stateId)->getName();
				$blocks[$name] = ($blocks[$name] ?? 0) + 1;
			}
		}
		foreach($subChunk->getBiomeArray()->getPalette() as $biomeId){
			$biomes[$biomeId] = true;
		}
	}
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
foreach($blocks as $name => $count){
	$parts[] = $name . " " . $count;
}
echo implode(", ", $parts), "\n";
ksort($biomes);
echo "biome ids present: ", implode(",", array_keys($biomes)), "\n";
echo "chunks holding minecraft:info_update: ", $withUnknown, "\n";
foreach($logger->records as [$level, $message]){
	echo "provider logged ", $level, ": ", $message, "\n";
}

if($onDisk === 0){
	$failures[] = "no chunks on disk";
}
if(count($missing) > 0){
	$failures[] = count($missing) . " spawn-area chunks missing or unpopulated: " . implode(" ", array_slice($missing, 0, 20));
}
if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
