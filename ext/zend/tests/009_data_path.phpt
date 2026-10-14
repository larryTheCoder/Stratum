--TEST--
A relative blob path is read against the data path the plugin sets, names the same world as the absolute one, and moves with the server directory
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
$server = sys_get_temp_dir() . "/stratum-data-path-" . getmypid();
$moved = $server . "-moved";
@mkdir("$server/worlds/w", 0o777, true);
$relative = "worlds/w/stratum-pipeline.blob";
$world = [$relative, "minecraft:overworld", "minecraft:overworld", 20260915];

// Before the data path is set, a relative path is refused by name wherever
// it would be read, and names nothing pinned or held.
echo "before setDataPath:\n";
attempt(fn() => Stratum\resolvePipelinePath($relative));
attempt(fn() => Stratum\freezePipeline($root, $relative));
attempt(fn() => Stratum\checkDimension($relative, "minecraft:overworld", "minecraft:overworld"));
attempt(fn() => Stratum\Dimension::open(...$world));
attempt(fn() => Stratum\pinWorld(...$world));
attempt(fn() => Stratum\unpinWorld(...$world));
attempt(fn() => Stratum\takeFluidUpdates(...$world, ...[0, 0]));
echo "a relative data path: ";
attempt(fn() => Stratum\setDataPath("server"));

Stratum\setDataPath($server);
echo "resolved: ", str_replace($server, "<server>", Stratum\resolvePipelinePath($relative)), "\n";
echo "normalised: ", str_replace($server, "<server>", Stratum\resolvePipelinePath("worlds/./x/../w/stratum-pipeline.blob")), "\n";
echo "absolute as it is: ", Stratum\resolvePipelinePath("/elsewhere/stratum-pipeline.blob"), "\n";

// Frozen, checked and opened through the relative path.
Stratum\freezePipeline($root, $relative);
var_dump(is_file("$server/$relative"));
attempt(fn() => Stratum\checkDimension($relative, "minecraft:overworld", "minecraft:overworld"));
$encoded = Stratum\Dimension::open(...$world)->encodeChunk(-3, 2);

// The relative path and the absolute one it names are one world: pinned by
// one, unpinned by the other, and a generator's updates left through one are
// taken through the other.
$absolute = ["$server/$relative", ...array_slice($world, 1)];
Stratum\pinWorld(...$world);
$worker = Stratum\Dimension::open(...$absolute);
$worker->encodeChunk(-3, 2);
unset($worker);
echo "taken through the relative path: ", count(Stratum\takeFluidUpdates(...$world, ...[-3, 2])) > 0 ? "yes" : "no", "\n";
echo "the data path may not move under a pinned world: ";
attempt(fn() => Stratum\setDataPath($moved));
echo "set again to itself: ";
attempt(fn() => Stratum\setDataPath($server));
var_dump(Stratum\unpinWorld(...$absolute), Stratum\unpinWorld(...$world));

// The server directory moves. The next process is handed its new data path,
// and the same options open the same blob where it now is; the absolute path
// an older world's options hold no longer does.
rename($server, $moved);
Stratum\setDataPath($moved);
var_dump(Stratum\Dimension::open(...$world)->encodeChunk(-3, 2) === $encoded);
echo "the old absolute path: ";
attempt(fn() => Stratum\checkDimension(...array_slice($absolute, 0, 3)));

unlink("$moved/$relative");
rmdir("$moved/worlds/w");
rmdir("$moved/worlds");
rmdir($moved);
?>
--EXPECTF--
before setDataPath:
the frozen pipeline path 'worlds/w/stratum-pipeline.blob' is relative to the server's data path, and none has been set (Stratum\setDataPath)
the frozen pipeline path 'worlds/w/stratum-pipeline.blob' is relative to the server's data path, and none has been set (Stratum\setDataPath)
the frozen pipeline path 'worlds/w/stratum-pipeline.blob' is relative to the server's data path, and none has been set (Stratum\setDataPath)
the frozen pipeline path 'worlds/w/stratum-pipeline.blob' is relative to the server's data path, and none has been set (Stratum\setDataPath)
the frozen pipeline path 'worlds/w/stratum-pipeline.blob' is relative to the server's data path, and none has been set (Stratum\setDataPath)
false
array (
)
a relative data path: the server's data path, which relative frozen pipeline paths are read against, must be absolute, not 'server'
resolved: <server>/worlds/w/stratum-pipeline.blob
normalised: <server>/worlds/w/stratum-pipeline.blob
absolute as it is: /elsewhere/stratum-pipeline.blob
bool(true)
ok
taken through the relative path: yes
the data path may not move under a pinned world: the server's data path cannot change from '%s' to '%s-moved' while a world is pinned: its relative frozen pipeline path would then name another file than the one it was pinned with
set again to itself: ok
bool(true)
bool(false)
bool(true)
the old absolute path: cannot read the frozen pipeline at %s/worlds/w/stratum-pipeline.blob
