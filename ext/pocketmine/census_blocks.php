<?php

declare(strict_types=1);

/*
 * Stratum — every Java block state Stratum's mapping can emit, through the
 * plugin's own BlockStateTranslator under PocketMine-MP 5.44.3's own
 * classes (M5 Stage 0; SPEC section 9).
 *
 *   php -d extension=stratum.so census_blocks.php --phar=PocketMine-MP.phar
 *       --version-root=.fixtures/1.21.11 [--report=FILE]
 *
 * Two questions, answered separately:
 *
 *  1. Over the WHOLE table (Stratum\javaBlockStateCount() states): how many
 *     does PocketMine-MP place as themselves, and which fall back? A
 *     fallback is what the translator says it is: it logs one warning per
 *     Java state through \GlobalLogger, naming the Bedrock block and why,
 *     and those warnings are what is counted. Every fallback is listed in
 *     --report. Not a failure in itself: the table covers every Java block,
 *     and v1 terrain places few of them.
 *
 *  2. Over what v1 terrain actually PLACES (the palettes of a spread of
 *     sampled chunks in each shipped dimension): any state that falls back
 *     to minecraft:info_update fails the check — that block would be in
 *     every world. One that falls back to the plugin's configured fallback
 *     (powder snow to snow) is that table's documented choice and is
 *     listed, not failed.
 *
 * Also counted, not failed: states PocketMine-MP takes but serializes back
 * as a different Bedrock state than it was given (a property it does not
 * model, normalised away), which is how a translation can be lossy without
 * any fallback — so the states placed as themselves are counted as those
 * that come back exactly as given and those that do not, and any that come
 * back as minecraft:air are named. The fallbacks are listed by Bedrock
 * block in three groups: blocks PocketMine-MP does not implement at all,
 * blocks it implements but not in the state given, and minecraft:unknown,
 * which is no block (the mapping's placeholder).
 *
 * Fails, as well, on anything escaping translate() — on a server that is an
 * exception on a generation worker — and on any log record above warning.
 */

namespace Stratum\PocketMineEvidence;

use pocketmine\data\bedrock\block\BlockStateData;
use pocketmine\nbt\tag\ByteTag;
use pocketmine\nbt\tag\IntTag;
use pocketmine\nbt\tag\StringTag;
use pocketmine\nbt\tag\Tag;
use pocketmine\world\format\io\GlobalBlockStateHandlers;
use Stratum\PMMP\BlockStateTranslator;

require __DIR__ . "/bootstrap.php";

$args = arguments($argv, ["phar", "version-root"], ["report"]);
bootstrap($args["phar"]);

$logger = new CapturingLogger();
\GlobalLogger::set($logger);

/** Java state id => "minecraft:block[prop=value,...]", from the version root's own report. */
function javaNames(string $versionRoot) : array{
	$report = json_decode((string) @file_get_contents($versionRoot . "/reports/blocks.json"), associative: true);
	if(!is_array($report)){
		fail("cannot read " . $versionRoot . "/reports/blocks.json (tools/fetch-vanilla writes it)");
	}
	$names = [];
	foreach($report as $block => $entry){
		foreach($entry["states"] as $state){
			$properties = [];
			foreach($state["properties"] ?? [] as $key => $value){
				$properties[] = $key . "=" . $value;
			}
			$names[$state["id"]] = $block . (count($properties) > 0 ? "[" . implode(",", $properties) . "]" : "");
		}
	}
	return $names;
}

function bedrockSpelling(array $bedrock) : string{
	$states = [];
	foreach($bedrock["states"] as $name => [, $value]){
		$states[] = $name . "=" . $value;
	}
	return $bedrock["name"] . (count($states) > 0 ? "[" . implode(",", $states) . "]" : "");
}

function tag(int $type, int|string $value) : Tag{
	return match($type){
		1 => new ByteTag((int) $value),
		3 => new IntTag((int) $value),
		8 => new StringTag((string) $value),
		default => fail("Stratum gave NBT tag type " . $type)
	};
}

$total = \Stratum\javaBlockStateCount();
$javaNames = javaNames($args["version-root"]);
if(count($javaNames) !== $total){
	fail("the version root's report has " . count($javaNames) . " block states and Stratum's table " . $total . ": they are not the same Minecraft version");
}

$translator = new BlockStateTranslator();
$upgrader = GlobalBlockStateHandlers::getUpgrader()->getBlockStateUpgrader();
$serializer = GlobalBlockStateHandlers::getSerializer();
$unknownId = GlobalBlockStateHandlers::getDeserializer()->deserialize(GlobalBlockStateHandlers::getUnknownBlockStateData());

/** @var array<int, int> $resolved Java id => PocketMine-MP state id */
$resolved = [];
/** @var array<int, array{string, string, string}> $fallbacks Java id => [Bedrock spelling, why, target] */
$fallbacks = [];
/** @var array<int, string> $escaped Java id => what escaped translate() */
$escaped = [];
/** @var array<int, string> $lossy Java id => "given -> serialized back" */
$lossy = [];
$failures = [];

$fallbackPattern = '/^Stratum: PocketMine-MP cannot place (\S+) \((.*)\); using (minecraft:info_update|this build\'s configured fallback) for it in generated terrain$/s';

for($javaId = 0; $javaId < $total; $javaId++){
	$bedrock = \Stratum\bedrockBlockState($javaId);
	$before = count($logger->records);
	try{
		$stateId = $translator->translate($javaId);
	}catch(\Throwable $e){
		$escaped[$javaId] = get_class($e) . ": " . $e->getMessage();
		continue;
	}
	$resolved[$javaId] = $stateId;
	$new = array_slice($logger->records, $before);
	if(count($new) === 0){
		// Translated: compare what PocketMine-MP would write back with
		// what it was given, after its own upgrade.
		$states = [];
		foreach($bedrock["states"] as $name => [$type, $value]){
			$states[$name] = tag($type, $value);
		}
		$given = $upgrader->upgrade(new BlockStateData($bedrock["name"], $states, $bedrock["version"]));
		$back = $serializer->serialize($stateId);
		if(!$back->equals($given)){
			$lossy[$javaId] = bedrockSpelling($bedrock) . " -> " . $back->getName() . "[" . implode(",", array_map(
				static fn(string $k, Tag $v) : string => $k . "=" . $v->getValue(),
				array_keys($back->getStates()), $back->getStates()
			)) . "]";
		}
		continue;
	}
	if(count($new) !== 1 || $new[0][0] !== \LogLevel::WARNING || preg_match($fallbackPattern, $new[0][1], $m) !== 1){
		$failures[] = "Java state " . $javaId . " (" . $javaNames[$javaId] . ") logged something other than one fallback warning: " . json_encode($new);
		continue;
	}
	$fallbacks[$javaId] = [bedrockSpelling($bedrock), $m[2], $m[3] === "minecraft:info_update" ? "minecraft:info_update" : "configured fallback"];
	if($m[3] === "minecraft:info_update" && $stateId !== $unknownId){
		$failures[] = "Java state " . $javaId . " was said to fall back to info_update but resolved to state " . $stateId;
	}
}

foreach($logger->records as [$level, $message]){
	if(in_array($level, [\LogLevel::EMERGENCY, \LogLevel::ALERT, \LogLevel::CRITICAL, \LogLevel::ERROR], true)){
		$failures[] = "logged at " . $level . ": " . $message;
	}
}

// --- the whole table --------------------------------------------------------

$toUnknown = count(array_filter($fallbacks, static fn(array $f) : bool => $f[2] === "minecraft:info_update"));
/** @var array<int, string> $toAir Java id => what was given: placed, but serialized back as minecraft:air */
$toAir = array_filter($lossy, static fn(string $change) : bool => str_contains($change, " -> minecraft:air["));
echo "Block state census, PocketMine-MP 5.44.3, through Stratum\\PMMP\\BlockStateTranslator\n";
echo "  Java block states in Stratum's table: ", $total, "\n";
echo "  placed as themselves (upgrader + deserializer): ", count($resolved) - count($fallbacks), ": ",
	count($resolved) - count($fallbacks) - count($lossy), " serialized back exactly as given, ",
	count($lossy), " as a different Bedrock state (lossy), ", count($toAir), " of those as minecraft:air\n";
foreach($toAir as $javaId => $change){
	echo "    as air: ", $javaNames[$javaId], ": ", $change, "\n";
}
echo "  fell back: ", count($fallbacks), " (", count($fallbacks) - $toUnknown, " to the plugin's configured fallback, ", $toUnknown, " to minecraft:info_update)\n";
echo "  escaped translate(): ", count($escaped), "\n";

/**
 * Each fallback's Bedrock block, by why: one PocketMine-MP does not
 * implement at all (its deserializer has no such id); one it implements, but
 * not in this state; or minecraft:unknown, which is no block — the mapping's
 * placeholder for a Java block Bedrock has no counterpart for.
 *
 * @var array<string, array<string, array{int, string, string, array<string, true>}>> $byBlock
 *      group => Bedrock name => [states, target, a reason, Java blocks]
 */
$byBlock = ["unimplemented" => [], "state" => [], "placeholder" => []];
foreach($fallbacks as $javaId => [$spelling, $why, $target]){
	$name = explode("[", $spelling, 2)[0];
	$group = match(true){
		$name === "minecraft:unknown" => "placeholder",
		str_starts_with($why, "Unknown block ID \"") => "unimplemented",
		default => "state"
	};
	$byBlock[$group][$name] ??= [0, $target, $why, []];
	$byBlock[$group][$name][0]++;
	$byBlock[$group][$name][3][explode("[", $javaNames[$javaId], 2)[0]] = true;
}
$headings = [
	"unimplemented" => "Bedrock blocks PocketMine-MP 5.44.3 does not implement",
	"state" => "Bedrock blocks it implements, but not in these states",
	"placeholder" => "no block at all: the mapping's placeholder for a Java block Bedrock has none for"
];
echo "  fallbacks by Bedrock block (", array_sum(array_map(count(...), $byBlock)), " blocks):\n";
foreach($headings as $group => $heading){
	ksort($byBlock[$group]);
	echo "   ", $heading, " (", count($byBlock[$group]), "):\n";
	foreach($byBlock[$group] as $name => [$count, $target, $why, $javaBlocks]){
		echo "    ", str_pad($name, 44), " ", str_pad((string) $count, 4, " ", STR_PAD_LEFT), " state(s) -> ", $target, "  (", $why, ")",
			$group === "unimplemented" ? "" : "  from Java " . implode(", ", array_keys($javaBlocks)), "\n";
	}
}
foreach($escaped as $javaId => $what){
	$failures[] = "translate(" . $javaId . ") for " . $javaNames[$javaId] . " threw " . $what;
}

// --- what v1 terrain places --------------------------------------------------

$blob = Sample::freeze($args["version-root"]);
$sampled = [];
foreach(Sample::DIMENSIONS as $label => [$settings, $biomes]){
	$dimension = \Stratum\Dimension::open($blob, $settings, $biomes, Sample::SEED);
	$chunks = $label === "overworld" ? Sample::grid(9, 41) : Sample::grid(5, 40);
	[$states] = Sample::palettes($dimension, $chunks);
	foreach($states as $javaId => $_){
		$sampled[$javaId][] = $label;
	}
	echo "Sampled ", $label, ": ", count($chunks), " chunks at seed ", Sample::SEED, ", ", count($states), " distinct Java states placed\n";
}
unlink($blob);
ksort($sampled);
$placedFallbacks = 0;
foreach($sampled as $javaId => $dimensions){
	if(!isset($fallbacks[$javaId])){
		continue;
	}
	$placedFallbacks++;
	[$spelling, $why, $target] = $fallbacks[$javaId];
	echo "  placed in ", implode("+", $dimensions), ": ", $javaNames[$javaId], " -> ", $spelling, " falls back to ", $target, " (", $why, ")\n";
	if($target === "minecraft:info_update"){
		$failures[] = "v1 terrain places " . $javaNames[$javaId] . ", and PocketMine-MP turns it into minecraft:info_update";
	}
}
echo "  of the placed states, ", $placedFallbacks, " fall back; ", count(array_intersect_key($lossy, $sampled)), " are serialized back differently\n";
foreach(array_intersect_key($lossy, $sampled) as $javaId => $change){
	echo "    ", $javaNames[$javaId], ": ", $change, "\n";
}

if(isset($args["report"])){
	$lines = ["# Java id\tJava state\tBedrock state\toutcome\twhy"];
	foreach($fallbacks as $javaId => [$spelling, $why, $target]){
		$lines[] = $javaId . "\t" . $javaNames[$javaId] . "\t" . $spelling . "\tfallback: " . $target . (isset($sampled[$javaId]) ? " (PLACED by v1 terrain)" : "") . "\t" . $why;
	}
	foreach($lossy as $javaId => $change){
		$lines[] = $javaId . "\t" . $javaNames[$javaId] . "\t" . $change . "\tserialized back differently" . (isset($sampled[$javaId]) ? " (PLACED by v1 terrain)" : "") . "\t-";
	}
	file_put_contents($args["report"], implode("\n", $lines) . "\n");
	echo "Every fallback and lossy state: ", $args["report"], "\n";
}

if(count($failures) > 0){
	foreach($failures as $failure){
		echo "FAIL: ", $failure, "\n";
	}
	exit(1);
}
echo "RESULT: PASS\n";
