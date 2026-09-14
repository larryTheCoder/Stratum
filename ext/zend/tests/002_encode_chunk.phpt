--TEST--
A chunk generated from a frozen vanilla pipeline loads into chunkutils2's own PalettedBlockArray
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

$dimension = Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:overworld", 20260915);
var_dump($dimension->getMinY(), $dimension->getHeight());

$count = Stratum\javaBlockStateCount();
foreach([[0, 0], [-3, 2]] as [$chunkX, $chunkZ]){
	$subChunks = $dimension->encodeChunk($chunkX, $chunkZ);
	echo "chunk $chunkX $chunkZ: ", count($subChunks), " sub-chunks, keys ", array_key_first($subChunks), " to ", array_key_last($subChunks), "\n";
	$empty = 0;
	$loaded = 0;
	foreach($subChunks as $index => ["blocks" => $blocks, "biomes" => $biomes]){
		// fromData validates the width, the exact word length, the palette
		// size and every offset: if it accepts the layer, the format is right.
		$biomeArray = PalettedBlockArray::fromData(...$biomes);
		if($blocks === null){
			$empty++;
			continue;
		}
		$blockArray = PalettedBlockArray::fromData(...$blocks);
		foreach($blockArray->getPalette() as $javaId){
			if(!is_int($javaId) || $javaId < 0 || $javaId >= $count){
				echo "sub-chunk $index holds $javaId, not a Java block state id\n";
			}
		}
		// Reading back through chunkutils2 gives the same ids the palette names.
		for($i = 0; $i < 64; $i++){
			$value = $blockArray->get($i % 16, intdiv($i, 3) % 16, intdiv($i, 7) % 16);
			if(!in_array($value, $blocks[2], true)){
				echo "sub-chunk $index read back $value\n";
			}
		}
		$loaded++;
	}
	echo "empty: ", $empty > 0 ? "some" : "none", ", loaded: ", $loaded > 0 ? "some" : "none", "\n";
}

// Every thread opening the same world gets the same compiled dimension; a
// second open must still work and agree.
$again = Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:overworld", 20260915);
var_dump($again->encodeChunk(0, 0) === $dimension->encodeChunk(0, 0));
unlink($blob);
?>
--EXPECT--
int(-64)
int(384)
chunk 0 0: 24 sub-chunks, keys -4 to 19
empty: some, loaded: some
chunk -3 2: 24 sub-chunks, keys -4 to 19
empty: some, loaded: some
bool(true)
