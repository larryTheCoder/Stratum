--TEST--
The End opens with its own biome source, and every quart of all 24 sub-chunks is Bedrock's one End biome
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
// The End generates y 0..127, sub-chunks 0..7, and every one of
// PocketMine-MP's other sixteen must still be given — a key Chunk's
// constructor does not find becomes an all-ocean biome array — so they come
// back as air, with the End's biome.
foreach([[0, 0], [2067, 0]] as [$chunkX, $chunkZ]){
	$subChunks = $end->encodeChunk($chunkX, $chunkZ);
	$palettes = [];
	$withBlocks = [];
	foreach($subChunks as $index => ["blocks" => $layer, "biomes" => $biomes]){
		PalettedBlockArray::fromData(...$biomes);
		$palettes[implode(",", $biomes[2])] = true;
		if($layer !== null){
			PalettedBlockArray::fromData(...$layer);
			$withBlocks[] = $index;
		}
	}
	echo "chunk $chunkX $chunkZ: ", count($subChunks), " sub-chunks, keys ", array_key_first($subChunks),
		" to ", array_key_last($subChunks), ", biome palettes ", implode(" ", array_keys($palettes)),
		", blocks only within 0..7: ", count($withBlocks) > 0 && min($withBlocks) >= 0 && max($withBlocks) <= 7 ? "yes" : "no",
		"\n";
}
unlink($blob);
?>
--EXPECT--
int(0)
int(128)
chunk 0 0: 24 sub-chunks, keys -4 to 19, biome palettes 9, blocks only within 0..7: yes
chunk 2067 0: 24 sub-chunks, keys -4 to 19, biome palettes 9, blocks only within 0..7: yes
