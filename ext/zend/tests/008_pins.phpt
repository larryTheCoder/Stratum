--TEST--
checkDimension refuses on the main thread what open would; a pinned world outlives its generators and its file
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
function attempt(callable $f) : void{
	try{
		$result = $f();
		echo $result === null ? "ok" : var_export($result, true), "\n";
	}catch(Stratum\GenerationException $e){
		echo $e->getMessage(), "\n";
	}
}

$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
Stratum\freezePipeline($root, $blob);
$garbage = tempnam(sys_get_temp_dir(), "stratum-blob-");
file_put_contents($garbage, "not a pipeline at all");

// The whole of opening, with no seed and nothing kept: what the plugin's
// world-load validation calls on the main thread.
echo "checkDimension:\n";
attempt(fn() => Stratum\checkDimension($blob, "minecraft:overworld", "minecraft:overworld"));
attempt(fn() => Stratum\checkDimension($blob, "minecraft:end", "minecraft:the_end"));
attempt(fn() => Stratum\checkDimension($blob, "minecraft:nether", "minecraft:nether"));
attempt(fn() => Stratum\checkDimension($blob, "minecraft:overworld", "minecraft:no_such_list"));
attempt(fn() => Stratum\checkDimension($blob, "minecraft:no_such_settings", "minecraft:overworld"));
attempt(fn() => Stratum\checkDimension($blob, "minecraft:overworld", "minecraft:fixed"));
attempt(fn() => Stratum\checkDimension($garbage, "minecraft:overworld", "minecraft:overworld"));
attempt(fn() => Stratum\checkDimension("/nonexistent/stratum.blob", "minecraft:overworld", "minecraft:overworld"));
// It keeps nothing: no outbox exists for any world afterwards.
echo "held after checks: ", count(Stratum\takeFluidUpdates($blob, "minecraft:overworld", "minecraft:overworld", 0, -3, 2)), "\n";

// A world pinned on the main thread, as the plugin does at world load.
$world = [$blob, "minecraft:overworld", "minecraft:overworld", 20260915];
echo "pin: ";
attempt(fn() => Stratum\pinWorld(...$world));

// A worker generates chunk (-3, 2), which holds aquifer fluid
// (004_fluid_updates.phpt), and is shut down while idle before PocketMine-MP
// populates the chunk. Unpinned, its updates went with it.
$worker = Stratum\Dimension::open(...$world);
$encoded = $worker->encodeChunk(-3, 2);
unset($worker);
$updates = Stratum\takeFluidUpdates(...$world, ...[-3, 2]);
echo "held after the worker went: ", count($updates) > 0 ? "yes" : "no", "\n";

// The blob is replaced on disk while the world is loaded. A restarted
// worker gets the state the world was pinned with, and the file is not read.
copy($garbage, $blob);
$restarted = Stratum\Dimension::open(...$world);
var_dump($restarted->encodeChunk(-3, 2) === $encoded);
var_dump(Stratum\takeFluidUpdates(...$world, ...[-3, 2]) === $updates);
unset($restarted);

// Pins are counted: two pins need two unpins.
Stratum\pinWorld(...$world);
var_dump(Stratum\unpinWorld(...$world), Stratum\unpinWorld(...$world), Stratum\unpinWorld(...$world));

// Unpinned and with no generator: nothing is held, and the next open reads
// the file, which now refuses.
echo "held after unpin: ", count(Stratum\takeFluidUpdates(...$world, ...[-3, 2])), "\n";
echo "open after unpin: ";
attempt(fn() => Stratum\Dimension::open(...$world));
// A pin that cannot open is refused with open's reason, and pins nothing.
echo "pin of a refused world: ";
attempt(fn() => Stratum\pinWorld(...$world));
var_dump(Stratum\unpinWorld(...$world));

unlink($garbage);
unlink($blob);
?>
--EXPECTF--
checkDimension:
ok
ok
%slegacy_random_source%s
the frozen pipeline has no biome parameter list 'minecraft:no_such_list'
the frozen pipeline has no noise settings 'minecraft:no_such_settings'
biome source 'minecraft:fixed' is not implemented by this build%s
not a frozen pipeline: %s
cannot read the frozen pipeline at /nonexistent/stratum.blob
held after checks: 0
pin: ok
held after the worker went: yes
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
held after unpin: 0
open after unpin: not a frozen pipeline: %s
pin of a refused world: not a frozen pipeline: %s
bool(false)
