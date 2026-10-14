--TEST--
The plugin's world-load validation opens the world in full on the main thread, and refuses there what a worker would have died on
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
// On PocketMine-MP an exception thrown on a generation worker takes the
// whole server down; the generator options validator (what Main registers
// with GeneratorManager::addGenerator, GeneratorOptions::validate) runs on
// the main thread as a world loads, and what it returns fails that world
// alone. So every refusal opening the world can make is made here, against
// a real frozen pipeline, by the real extension: the single PocketMine-MP
// class this touches is a stand-in (001_generator_options.phpt says why).
namespace pocketmine\world\generator {
	class InvalidGeneratorOptionsException extends \UnexpectedValueException{}
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";

	use Stratum\PMMP\GeneratorOptions;

	$root = getenv("STRATUM_FIXTURES_DIR") . "/1.21.11";
	$blob = tempnam(sys_get_temp_dir(), "stratum-blob-");
	Stratum\freezePipeline($root, $blob);

	// The same pipeline, stamped by another pipeline engine version: the
	// load-bearing refusal of SPEC section 6, which used to be made only on
	// a worker, by Dimension::open.
	$bytes = file_get_contents($blob);
	$restamped = tempnam(sys_get_temp_dir(), "stratum-blob-");
	file_put_contents($restamped, substr_replace($bytes, pack("V", unpack("V", $bytes, 12)[1] + 1), 12, 4));

	foreach([
		"the overworld" => GeneratorOptions::encode($blob),
		"large biomes" => GeneratorOptions::encode($blob, "minecraft:large_biomes", "minecraft:overworld"),
		"amplified" => GeneratorOptions::encode($blob, "minecraft:amplified", "minecraft:overworld"),
		"the End" => GeneratorOptions::encode($blob, "minecraft:end", "minecraft:the_end"),
		// A pairing vanilla does not make is taken as asked, and opens.
		"the End's terrain, the overworld's biomes" => GeneratorOptions::encode($blob, "minecraft:end", "minecraft:overworld"),
		// Each of these opened only on a worker before, and took the
		// server down there.
		"the legacy Nether" => GeneratorOptions::encode($blob, "minecraft:nether", "minecraft:nether"),
		"unknown settings" => GeneratorOptions::encode($blob, "minecraft:no_such_settings", "minecraft:overworld"),
		"unknown parameter list" => GeneratorOptions::encode($blob, "minecraft:overworld", "minecraft:no_such_list"),
		"settings that are no identifier" => GeneratorOptions::encode($blob, "Minecraft:Overworld", "minecraft:overworld"),
		"another engine version" => GeneratorOptions::encode($restamped),
	] as $what => $options){
		$e = GeneratorOptions::validate($options);
		echo $what, ": ", $e === null ? "accepted" : get_class($e) . ": " . str_replace([$restamped, $blob], ["<restamped>", "<blob>"], $e->getMessage()), "\n";
	}

	// check() hands back what it accepted, read from the string as a
	// worker's parse() reads it.
	$checked = GeneratorOptions::check(GeneratorOptions::encode($blob, "minecraft:end", "minecraft:the_end"));
	var_dump($checked->blobPath === $blob, $checked->noiseSettings, $checked->biomeSource);

	// A pack whose noise settings name a block state the mapping table
	// cannot place froze and validated, and threw from the encoder at the
	// first chunk holding it, on a worker. Three such settings beside
	// vanilla's own, in a scratch copy of the version root: the overworld's
	// with a property stone does not have, with a block that does not exist,
	// and with its surface rules' grass block misspelt.
	$scratch = sys_get_temp_dir() . "/stratum-unmapped-" . getmypid();
	$copy = function(string $from, string $to) use (&$copy) : void{
		@mkdir($to, 0o777, true);
		foreach(array_diff(scandir($from), [".", ".."]) as $name){
			is_dir("$from/$name") ? $copy("$from/$name", "$to/$name") : copy("$from/$name", "$to/$name");
		}
	};
	foreach(["density_function", "noise", "noise_settings", "biome"] as $registry){
		$copy("$root/worldgen/$registry", "$scratch/worldgen/$registry");
	}
	$copy("$root/biome_parameters", "$scratch/biome_parameters");
	$overworld = json_decode(file_get_contents("$root/worldgen/noise_settings/overworld.json"), true);
	$settings = "$scratch/worldgen/noise_settings";
	file_put_contents("$settings/snowy_stone.json", json_encode(["default_block" => ["Name" => "minecraft:stone", "Properties" => ["snowy" => "true"]]] + $overworld));
	file_put_contents("$settings/not_a_block.json", json_encode(["default_block" => ["Name" => "minecraft:not_a_block"]] + $overworld));
	$misspelt = str_replace('"minecraft:grass_block"', '"minecraft:grass_blok"', json_encode($overworld), $count);
	echo "grass blocks misspelt: ", $count > 0 ? "yes" : "none found", "\n";
	file_put_contents("$settings/misspelt_surface.json", $misspelt);
	$unmapped = "$scratch/stratum-pipeline.blob";
	Stratum\freezePipeline($scratch, $unmapped);
	foreach(["snowy_stone", "not_a_block", "misspelt_surface"] as $name){
		$e = GeneratorOptions::validate(GeneratorOptions::encode($unmapped, "minecraft:$name", "minecraft:overworld"));
		echo $name, ": ", $e === null ? "accepted" : str_replace($unmapped, "<blob>", $e->getMessage()), "\n";
	}
	// Vanilla's own, from the same blob, open.
	var_dump(GeneratorOptions::validate(GeneratorOptions::encode($unmapped)));
	$remove = function(string $path) use (&$remove) : void{
		if(is_dir($path)){
			foreach(array_diff(scandir($path), [".", ".."]) as $name){
				$remove("$path/$name");
			}
			rmdir($path);
		}else{
			unlink($path);
		}
	};
	$remove($scratch);

	unlink($restamped);
	unlink($blob);
}
?>
--EXPECTF--
the overworld: accepted
large biomes: accepted
amplified: accepted
the End: accepted
the End's terrain, the overworld's biomes: accepted
the legacy Nether: pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate this world from its frozen pipeline at <blob>: %slegacy_random_source%s
unknown settings: pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate this world from its frozen pipeline at <blob>: the frozen pipeline has no noise settings 'minecraft:no_such_settings'
unknown parameter list: pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate this world from its frozen pipeline at <blob>: the frozen pipeline has no biome parameter list 'minecraft:no_such_list'
settings that are no identifier: pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate this world from its frozen pipeline at <blob>: invalid character 'M' at position 0 of the namespace in resource location 'Minecraft'
another engine version: pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate this world from its frozen pipeline at <restamped>: this world was frozen by pipeline engine version %d and this build is version %d, which is not guaranteed to generate the same terrain.%s
bool(true)
string(13) "minecraft:end"
string(17) "minecraft:the_end"
grass blocks misspelt: yes
snowy_stone: Stratum cannot generate this world from its frozen pipeline at <blob>: noise settings 'minecraft:snowy_stone' name a block state this build's mapping table cannot place: default_block minecraft:stone[snowy=true] (stratum-mapping: 'minecraft:stone' has no property 'snowy' (it has: none))
not_a_block: Stratum cannot generate this world from its frozen pipeline at <blob>: noise settings 'minecraft:not_a_block' name a block state this build's mapping table cannot place: default_block minecraft:not_a_block (stratum-mapping: 'minecraft:not_a_block' is not a block at Minecraft 1.21.11)
misspelt_surface: Stratum cannot generate this world from its frozen pipeline at <blob>: noise settings 'minecraft:misspelt_surface' name a block state this build's mapping table cannot place: surface rule result_state minecraft:grass_blok[snowy=false] (stratum-mapping: 'minecraft:grass_blok' is not a block at Minecraft 1.21.11)
NULL
