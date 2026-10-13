--TEST--
The End opens with its own biome source, and every quart of it is Bedrock's one End biome
--EXTENSIONS--
stratum
chunkutils2
--SKIPIF--
<?php
$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
if(!is_dir("$root/worldgen/noise_settings") || !is_dir("$root/biome_parameters")){
	die("skip no vanilla fixtures; run tools/fetch-vanilla");
}
?>
--FILE--
<?php
use pocketmine\world\format\PalettedBlockArray;

$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
Stratum\freezePipeline($root, $blob);

// minecraft:the_end is the End's biome source, not a parameter list: Java's
// five End biomes come out of it (tests/conformance/
// vanilla_end_biome_source_test.cpp holds them to the server), and Bedrock
// has one End biome, id 9, for all five.
$end = Stratum\Dimension::open($blob, "minecraft:end", "minecraft:the_end", 42);
var_dump($end->getMinY(), $end->getHeight());

// (0, 0) is the central island's; (2067, 0) is 33 000 blocks out, where the
// server stores end_highlands over an outer island (end-islands r.64.0).
foreach([[0, 0], [2067, 0]] as [$chunkX, $chunkZ]){
	$subChunks = $end->encodeChunk($chunkX, $chunkZ);
	$palettes = [];
	$blocks = 0;
	foreach($subChunks as ["blocks" => $layer, "biomes" => $biomes]){
		PalettedBlockArray::fromData(...$biomes);
		$palettes[implode(",", $biomes[2])] = true;
		$blocks += $layer === null ? 0 : 1;
	}
	echo "chunk $chunkX $chunkZ: ", count($subChunks), " sub-chunks, keys ", array_key_first($subChunks),
		" to ", array_key_last($subChunks), ", biome palettes ", implode(" ", array_keys($palettes)),
		", non-air sub-chunks ", $blocks > 0 ? "some" : "none", "\n";
}
unlink($blob);
?>
--EXPECT--
int(0)
int(128)
chunk 0 0: 8 sub-chunks, keys 0 to 7, biome palettes 9, non-air sub-chunks some
chunk 2067 0: 8 sub-chunks, keys 0 to 7, biome palettes 9, non-air sub-chunks some
