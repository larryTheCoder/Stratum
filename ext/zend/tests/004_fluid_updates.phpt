--TEST--
A chunk's fluid updates reach the main thread once, as world positions, by the world's options and seed
--EXTENSIONS--
stratum
--SKIPIF--
<?php
$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
if(!is_dir("$root/worldgen/noise_settings") || !is_dir("$root/biome_parameters")){
	die("skip no vanilla fixtures; run tools/fetch-vanilla");
}
?>
--FILE--
<?php
$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
Stratum\freezePipeline($root, $blob);
$world = [$blob, "minecraft:overworld", "minecraft:overworld", 20260915];

// Nothing is open for this world yet: nothing to take, and asking opens or
// compiles nothing.
var_dump(Stratum\takeFluidUpdates(...$world, ...[-3, 2]));

$dimension = Stratum\Dimension::open(...$world);
$minY = $dimension->getMinY();
$maxY = $minY + $dimension->getHeight();

// Chunk (-3, 2) holds aquifer fluid that ticks (ext/tests/
// vanilla_chunk_encoder_test.cpp says the same of the C++ side); (0, 0) none.
$dimension->encodeChunk(-3, 2);
$dimension->encodeChunk(0, 0);
$updates = Stratum\takeFluidUpdates(...$world, ...[-3, 2]);
echo "chunk -3 2: ", count($updates) > 0 ? "some" : "none", "\n";
$outside = 0;
foreach($updates as [$x, $y, $z]){
	// World coordinates inside the chunk: x -48..-33, z 32..47.
	if($x < -48 || $x > -33 || $z < 32 || $z > 47 || $y < $minY || $y >= $maxY){
		$outside++;
	}
}
echo "outside the chunk: $outside\n";
echo "distinct: ", count(array_unique(array_map(fn($p) => implode(",", $p), $updates))) === count($updates) ? "yes" : "no", "\n";
echo "chunk 0 0: ", count(Stratum\takeFluidUpdates(...$world, ...[0, 0])), "\n";

// Taken once: a second take of the same chunk is empty.
echo "taken again: ", count(Stratum\takeFluidUpdates(...$world, ...[-3, 2])), "\n";

// Encoding the chunk again replaces what it left rather than adding to it.
$dimension->encodeChunk(-3, 2);
$dimension->encodeChunk(-3, 2);
$again = Stratum\takeFluidUpdates(...$world, ...[-3, 2]);
var_dump($again === $updates);

// Another seed, or another blob path, is another world.
$dimension->encodeChunk(-3, 2);
echo "other seed: ", count(Stratum\takeFluidUpdates($blob, "minecraft:overworld", "minecraft:overworld", 1, -3, 2)), "\n";
echo "other blob: ", count(Stratum\takeFluidUpdates($blob . "x", "minecraft:overworld", "minecraft:overworld", 20260915, -3, 2)), "\n";

// Once the world's last Dimension is gone, so is what it left.
unset($dimension);
echo "after unload: ", count(Stratum\takeFluidUpdates(...$world, ...[-3, 2])), "\n";
unlink($blob);
?>
--EXPECT--
array(0) {
}
chunk -3 2: some
outside the chunk: 0
distinct: yes
chunk 0 0: 0
taken again: 0
bool(true)
other seed: 0
other blob: 0
after unload: 0
