<?php

declare(strict_types=1);

/*
 * Stratum — every biome id Stratum can emit, for each dimension vanilla's
 * world presets name, against PocketMine-MP 5.44.3's own biome maps (M5
 * Stage 0; SPEC section 9).
 *
 *   php -d extension=stratum.so census_biomes.php --phar=PocketMine-MP.phar
 *       --version-root=.fixtures/1.21.11 --biome-table=lib/mapping/src/biome_table.inc
 *
 * Which biomes a dimension can emit is read the way the engine reads it:
 * every world preset in the version root (worldgen/world_preset/), each
 * dimension's biome source — a multi_noise preset's parameter list
 * (biome_parameters/), or the End's own source, whose five biomes are
 * fixed. Each Java biome becomes the Bedrock id Stratum's table gives it
 * (lib/mapping/src/biome_table.inc, the very table compiled into
 * stratum.so; parsed here because the module exposes no lookup by name),
 * and then:
 *
 *  - LegacyBiomeIdToStringIdMap must know the id. It is what
 *    ChunkSerializer asks on the wire, and an id it does not know is sent
 *    to clients as OCEAN, with no exception and no log (FAIL).
 *  - Its name must be in biome_definitions.json, the BiomeDefinitionList
 *    PocketMine-MP sends every client (FAIL).
 *  - BiomeRegistry is reported, not failed: PocketMine-MP registers only
 *    the dozen biomes its own generator uses, and answers UnknownBiome for
 *    the rest, which affects nothing a generator hands it.
 *
 * And as a check on the parse: every biome id a spread of real encoded
 * chunks carries, in each shipped dimension, must be one this census
 * predicted for that dimension (FAIL otherwise).
 */

namespace Stratum\PocketMineEvidence;

use pocketmine\data\bedrock\LegacyBiomeIdToStringIdMap;
use pocketmine\world\biome\BiomeRegistry;
use pocketmine\world\biome\UnknownBiome;
use const pocketmine\BEDROCK_DATA_PATH;

require __DIR__ . "/bootstrap.php";

$args = arguments($argv, ["phar", "version-root", "biome-table"]);
bootstrap($args["phar"]);
$root = $args["version-root"];

/** @return array<string, int> Java biome => Bedrock id */
function biomeTable(string $path) : array{
	$source = (string) @file_get_contents($path);
	if(preg_match('/std::array<BiomeMapping, (\d+)> kBiomeTable/', $source, $size) !== 1){
		fail("cannot find kBiomeTable's size in " . $path);
	}
	preg_match_all('/\{\.javaId = "([a-z0-9_.\/-]+:[a-z0-9_.\/-]+)", \.bedrockId = (\d+)\}/', $source, $rows, PREG_SET_ORDER);
	$table = [];
	foreach($rows as [, $java, $bedrock]){
		$table[$java] = (int) $bedrock;
	}
	if(count($table) !== (int) $size[1]){
		fail("read " . count($table) . " rows from " . $path . ", which declares " . $size[1]);
	}
	return $table;
}

function readJson(string $path) : array{
	$decoded = json_decode((string) @file_get_contents($path), associative: true);
	if(!is_array($decoded)){
		fail("cannot read " . $path);
	}
	return $decoded;
}

function resourcePath(string $id) : array{
	$parts = explode(":", $id, 2);
	return count($parts) === 2 ? $parts : ["minecraft", $parts[0]];
}

$table = biomeTable($args["biome-table"]);
$endBiomes = ["minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands", "minecraft:small_end_islands", "minecraft:end_barrens"];

/** @var array<string, array{list<string>, list<string>}> $sources biome source => [Java biomes, presets naming it] */
$sources = [];
$presets = glob($root . "/worldgen/world_preset/*.json");
if($presets === false || count($presets) === 0){
	fail("no world presets under " . $root . "/worldgen/world_preset");
}
sort($presets);
$notEmitted = [];
foreach($presets as $presetFile){
	$preset = "minecraft:" . basename($presetFile, ".json");
	foreach(readJson($presetFile)["dimensions"] ?? [] as $dimension => $entry){
		$where = $preset . "'s " . $dimension;
		$source = $entry["generator"]["biome_source"] ?? null;
		$type = $source["type"] ?? null;
		if($type === "minecraft:multi_noise" && is_string($source["preset"] ?? null)){
			[$namespace, $path] = resourcePath($source["preset"]);
			$list = readJson($root . "/biome_parameters/" . $namespace . "/" . $path . ".json");
			$key = $namespace . ":" . $path;
			$sources[$key] ??= [array_values(array_unique(array_map(static fn(array $b) : string => $b["biome"], $list["biomes"]))), []];
			$sources[$key][1][] = $where;
		}elseif($type === "minecraft:the_end"){
			$sources["minecraft:the_end"] ??= [$endBiomes, []];
			$sources["minecraft:the_end"][1][] = $where;
		}else{
			// flat, debug, fixed: biome sources this build refuses by name
			// (Stratum\parseBiomeSource), so nothing it can emit.
			$notEmitted[] = $where . " (" . ($type ?? $entry["generator"]["type"] ?? "?") . ")";
		}
	}
}

$wire = LegacyBiomeIdToStringIdMap::getInstance();
$definitions = readJson(BEDROCK_DATA_PATH . "biome_definitions.json");
$registry = BiomeRegistry::getInstance();
$failures = [];
/** @var array<string, array<int, true>> $predicted biome source => Bedrock ids */
$predicted = [];

echo "Biome census, PocketMine-MP 5.44.3: LegacyBiomeIdToStringIdMap (the wire), biome_definitions.json (sent to clients), BiomeRegistry\n";
ksort($sources);
foreach($sources as $source => [$biomes, $where]){
	sort($biomes);
	echo "  ", $source, ", named by ", implode(", ", array_unique($where)), ": ", count($biomes), " Java biomes",
		$source === "minecraft:nether" ? " (the Nether does not ship yet: its surface rules' named noises are refused; checked all the same)" : "", "\n";
	foreach($biomes as $java){
		if(!isset($table[$java])){
			$failures[] = $java . " (" . $source . ") is not in Stratum's biome table";
			continue;
		}
		$id = $table[$java];
		$predicted[$source][$id] = true;
		$name = $wire->legacyToString($id);
		$defined = $name !== null && isset($definitions["minecraft:" . $name]);
		$known = !($registry->getBiome($id) instanceof UnknownBiome);
		echo "    ", str_pad($java, 36), " -> ", str_pad((string) $id, 3, " ", STR_PAD_LEFT), " ", str_pad($name ?? "UNKNOWN (sent as OCEAN)", 24),
			$defined ? " defined" : " NOT DEFINED", $known ? ", BiomeRegistry knows it" : ", BiomeRegistry: UnknownBiome", "\n";
		if($name === null){
			$failures[] = $java . " -> " . $id . ": LegacyBiomeIdToStringIdMap does not know it, so clients are sent OCEAN";
		}elseif(!$defined){
			$failures[] = $java . " -> " . $id . " (" . $name . "): not in biome_definitions.json";
		}
	}
	echo "    distinct Bedrock ids: ", count($predicted[$source] ?? []), "\n";
}
foreach($notEmitted as $where){
	echo "  not a source this build emits from: ", $where, "\n";
}

// The parse, checked against what the engine really emits.
$blob = Sample::freeze($root);
$checked = ["overworld" => "minecraft:overworld", "end" => "minecraft:the_end"];
foreach(Sample::DIMENSIONS as $label => [$settings, $biomeSource]){
	$dimension = \Stratum\Dimension::open($blob, $settings, $biomeSource, Sample::SEED);
	$chunks = $label === "overworld" ? Sample::grid(9, 41) : Sample::grid(5, 40);
	[, $seen] = Sample::palettes($dimension, $chunks);
	ksort($seen);
	$unpredicted = array_diff_key($seen, $predicted[$checked[$label]] ?? []);
	echo "  encoded ", $label, " (", count($chunks), " chunks at seed ", Sample::SEED, "): ids ", implode(",", array_keys($seen)),
		count($unpredicted) === 0 ? ", every one predicted\n" : ", NOT PREDICTED: " . implode(",", array_keys($unpredicted)) . "\n";
	if(count($unpredicted) > 0){
		$failures[] = "encoded " . $label . " chunks carry biome ids this census did not predict: " . implode(",", array_keys($unpredicted));
	}
}
unlink($blob);

if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
