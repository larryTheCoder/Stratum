--TEST--
A blob restamped or damaged is refused by name even while its intact twin is open and shared
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
// Compiled dimensions are shared across every thread that opens the same
// world. They used to be shared by the header's content hash, which covers
// the payload alone, and freeze::read — where the container format, the
// pipeline engine version and the Minecraft pin are refused (SPEC §6) —
// ran only on a miss. So once a world was open, a blob with the same
// payload under another engine version matched it, and was handed its
// dimension: a world generating from a pipeline this build may not
// reproduce, with nothing said. Shared by every byte, each of these misses
// and is read, and refused, afresh.
function attempt(callable $f) : void{
	try{
		$f();
		echo "no exception\n";
	}catch(Stratum\GenerationException $e){
		echo $e->getMessage(), "\n";
	}
}

$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
Stratum\freezePipeline($root, $blob);
$bytes = file_get_contents($blob);

// Held for the whole test: its dimension is in the registry throughout.
$world = Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:overworld", 20260915);

// The header: "STRTMPIP", then the container format and the pipeline engine
// version as little-endian u32, then the Minecraft pin as a u32 length and
// its characters, then the payload's content hash.
$format = unpack("V", $bytes, 8)[1];
$engine = unpack("V", $bytes, 12)[1];
$pinLength = unpack("V", $bytes, 16)[1];
$pinAt = 20 + $pinLength - 1;
$variants = [
	"another engine version" => substr_replace($bytes, pack("V", $engine + 1), 12, 4),
	"another container format" => substr_replace($bytes, pack("V", $format + 1), 8, 4),
	"another Minecraft pin" => substr_replace($bytes, $bytes[$pinAt] === "9" ? "8" : "9", $pinAt, 1),
	"a payload damaged under its header" => substr_replace($bytes, chr(ord($bytes[strlen($bytes) - 1]) ^ 1), -1, 1),
];
foreach($variants as $what => $variant){
	$path = tempnam(sys_get_temp_dir(), "stratum-blob-");
	file_put_contents($path, $variant);
	echo $what, ":\n  open: ";
	attempt(fn() => Stratum\Dimension::open($path, "minecraft:overworld", "minecraft:overworld", 20260915));
	// The main thread's check refuses it the same way.
	echo "  check: ";
	attempt(fn() => Stratum\checkDimension($path, "minecraft:overworld", "minecraft:overworld"));
	unlink($path);
}

// The intact bytes at another path still share, and generate the same.
$twin = tempnam(sys_get_temp_dir(), "stratum-blob-");
file_put_contents($twin, $bytes);
$again = Stratum\Dimension::open($twin, "minecraft:overworld", "minecraft:overworld", 20260915);
var_dump($again->encodeChunk(0, 0) === $world->encodeChunk(0, 0));
attempt(fn() => Stratum\checkDimension($twin, "minecraft:overworld", "minecraft:overworld"));
unlink($twin);
unlink($blob);
?>
--EXPECTF--
another engine version:
  open: this world was frozen by pipeline engine version %d and this build is version %d, which is not guaranteed to generate the same terrain.%s
  check: this world was frozen by pipeline engine version %d and this build is version %d, which is not guaranteed to generate the same terrain.%s
another container format:
  open: this world's pipeline is stored in container format %d and this build writes %d; it cannot be read
  check: this world's pipeline is stored in container format %d and this build writes %d; it cannot be read
another Minecraft pin:
  open: this world was frozen against the Minecraft %s schema and this build is pinned to %s
  check: this world was frozen against the Minecraft %s schema and this build is pinned to %s
a payload damaged under its header:
  open: this world's pipeline does not match its own content hash;%s
  check: this world's pipeline does not match its own content hash;%s
bool(true)
no exception
