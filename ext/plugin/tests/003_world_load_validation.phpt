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

	unlink($restamped);
	unlink($blob);
}
?>
--EXPECTF--
the overworld: accepted
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
