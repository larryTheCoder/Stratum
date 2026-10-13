--TEST--
What cannot be generated is refused by name, as a GenerationException
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
		$f();
		echo "no exception\n";
	}catch(Stratum\GenerationException $e){
		echo $e->getMessage(), "\n";
	}
}

attempt(fn() => Stratum\Dimension::open("/nonexistent/stratum.blob", "minecraft:overworld", "minecraft:overworld", 1));

$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
file_put_contents($blob, "not a pipeline at all");
attempt(fn() => Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:overworld", 1));

Stratum\freezePipeline(getenv("STRATUM_FIXTURES_DIR") . "/1.21.11", $blob);
attempt(function() use ($blob){
	Stratum\Dimension::open($blob, "minecraft:nether", "minecraft:nether", 1);
});
attempt(fn() => Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:no_such_list", 1));
// The End's settings with no list of their own: no list is called
// minecraft:end, and its source is spelled minecraft:the_end.
attempt(fn() => Stratum\Dimension::open($blob, "minecraft:end", "minecraft:end", 1));
// A biome source type this build does not implement, refused as one.
attempt(fn() => Stratum\Dimension::open($blob, "minecraft:overworld", "minecraft:fixed", 1));
unlink($blob);
?>
--EXPECTF--
cannot read the frozen pipeline at /nonexistent/stratum.blob
not a frozen pipeline: %s
%slegacy_random_source%s
the frozen pipeline has no biome parameter list 'minecraft:no_such_list'
the frozen pipeline has no biome parameter list 'minecraft:end'
biome source 'minecraft:fixed' is not implemented by this build%s
