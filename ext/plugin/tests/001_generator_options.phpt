--TEST--
The plugin's generator options refuse, by name, anything that cannot generate a world
--FILE--
<?php
// PocketMine-MP is not installable here (its own PHP ships no headers and it
// needs a stack of extensions), so this exercises the one plugin class that
// is pure PHP against a faithful stand-in for the single PMMP class it
// touches: InvalidGeneratorOptionsException really is nothing more than an
// empty subclass of UnexpectedValueException
// (src/world/generator/InvalidGeneratorOptionsException.php:26).
namespace pocketmine\world\generator {
	class InvalidGeneratorOptionsException extends \UnexpectedValueException{}
}

namespace {
	require __DIR__ . "/../src/GeneratorOptions.php";

	use Stratum\PMMP\GeneratorOptions;

	$blob = tempnam(sys_get_temp_dir(), "stratum-options-");
	file_put_contents($blob, "not really a pipeline, but readable");

	$good = GeneratorOptions::encode($blob);
	$decoded = json_decode($good, associative: true);
	echo "encoded keys: ", implode(",", array_keys($decoded)), "\n";
	var_dump($decoded["blob"] === $blob);

	$parsed = GeneratorOptions::parse($good);
	var_dump($parsed->blobPath === $blob, $parsed->noiseSettings, $parsed->biomeSource);
	var_dump(GeneratorOptions::validate($good));

	// The End, with its own biome source.
	$end = GeneratorOptions::parse(GeneratorOptions::encode($blob, "minecraft:end", "minecraft:the_end"));
	var_dump($end->noiseSettings, $end->biomeSource);

	// Every refusal names what is wrong: these messages are what a server
	// operator sees when a world will not load.
	foreach([
		"" ,
		"not json",
		'{"settings":"minecraft:overworld"}',
		'{"blob":"/nonexistent/stratum.blob"}',
		'{"blob":' . json_encode($blob) . ',"settings":42,"biomes":"minecraft:overworld"}',
		// Settings with no biome source beside them would pair the End with
		// the overworld's biomes, quietly.
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:end"}'
	] as $bad){
		$e = GeneratorOptions::validate($bad);
		echo get_class($e), ": ", str_replace($blob, "<blob>", $e->getMessage()), "\n";
	}
	unlink($blob);
}
?>
--EXPECTF--
encoded keys: blob,settings,biomes
bool(true)
bool(true)
string(19) "minecraft:overworld"
string(19) "minecraft:overworld"
NULL
string(13) "minecraft:end"
string(17) "minecraft:the_end"
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum needs a frozen pipeline to generate from.%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options are not a JSON object%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options have no "blob" path to a frozen pipeline:%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot read this world's frozen pipeline at /nonexistent/stratum.blob
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's "settings" and "biomes" options must be strings:%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options name "settings" without "biomes", the biome source to go with them%s
