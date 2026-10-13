--TEST--
The plugin's generator options refuse, by name, anything that cannot generate a world
--EXTENSIONS--
stratum
--FILE--
<?php
// PocketMine-MP is not installable here (its own PHP ships no headers and it
// needs a stack of extensions), so this exercises the plugin's options class
// against a faithful stand-in for the single PMMP class it touches:
// InvalidGeneratorOptionsException really is nothing more than an empty
// subclass of UnexpectedValueException
// (src/world/generator/InvalidGeneratorOptionsException.php:26). The
// extension itself is real: the biome source is checked by its own
// Stratum\parseBiomeSource, as on a server, where plugin.yml requires it.
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
	// A bare path is in the minecraft namespace, as parse reads it; the
	// options keep the spelling they were given.
	var_dump(GeneratorOptions::parse(GeneratorOptions::encode($blob, "minecraft:end", "the_end"))->biomeSource);
	// A pairing vanilla does not make is the caller's to ask for, and is
	// taken as asked: a datapack may pair settings with any source.
	var_dump(GeneratorOptions::validate(GeneratorOptions::encode($blob, "minecraft:end", "minecraft:overworld")));
	// encode() takes the pair together too: half of it is refused here, as
	// parse() refuses it, rather than defaulted to the overworld's half.
	foreach([["minecraft:end", null], [null, "minecraft:the_end"]] as [$settings, $biomes]){
		try{
			GeneratorOptions::encode($blob, $settings, $biomes);
			echo "encode accepted half a pair\n";
		}catch(\InvalidArgumentException $e){
			echo "encode refused: ", $e->getMessage(), "\n";
		}
	}

	// Every refusal names what is wrong: these messages are what a server
	// operator sees when a world will not load.
	foreach([
		"" ,
		"not json",
		'{"settings":"minecraft:overworld"}',
		'{"blob":"/nonexistent/stratum.blob"}',
		// A directory is readable, and no pipeline: refused here, not on a
		// worker.
		'{"blob":' . json_encode(sys_get_temp_dir()) . '}',
		'{"blob":' . json_encode($blob) . ',"settings":42,"biomes":"minecraft:overworld"}',
		// Settings with no biome source beside them would pair the End with
		// the overworld's biomes, quietly.
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:end"}',
		// And a biome source with no settings would pair the End's biomes
		// with the overworld's terrain, as quietly.
		'{"blob":' . json_encode($blob) . ',"biomes":"minecraft:the_end"}',
		// A null is a key that is present, not an absent one: read as
		// absent it would become the overworld's half of the pair, the
		// overworld's terrain under the End's biomes and the End's terrain
		// under the overworld's.
		'{"blob":' . json_encode($blob) . ',"settings":null,"biomes":"minecraft:the_end"}',
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:end","biomes":null}',
		// A misspelt key would be ignored, and the world would be the
		// overworld's.
		'{"blob":' . json_encode($blob) . ',"biome":"minecraft:the_end"}',
		// What world::BiomeSource::parse refuses is refused here, with its
		// reason, before any worker opens the pipeline.
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:overworld","biomes":"minecraft:fixed"}',
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:overworld","biomes":"minecraft:checkerboard"}',
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:overworld","biomes":"minecraft:multi_noise"}',
		'{"blob":' . json_encode($blob) . ',"settings":"minecraft:end","biomes":"Minecraft:The_End"}'
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
string(7) "the_end"
NULL
encode refused: GeneratorOptions::encode() was given noise settings (minecraft:end) without the biome source to go with them
encode refused: GeneratorOptions::encode() was given a biome source (minecraft:the_end) without the noise settings to go with it
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum needs a frozen pipeline to generate from.%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options are not a JSON object%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options have no "blob" path to a frozen pipeline:%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot read this world's frozen pipeline at /nonexistent/stratum.blob
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot read this world's frozen pipeline at %s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's "settings" and "biomes" options must be strings:%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options name "settings" without "biomes", the biome source to go with them%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options name "biomes" without "settings", the noise settings to go with them%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's "settings" and "biomes" options must be strings: {"blob":%s,"settings":null,"biomes":"minecraft:the_end"}
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's "settings" and "biomes" options must be strings: {"blob":%s,"settings":"minecraft:end","biomes":null}
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum's generator options take "blob", "settings" and "biomes" and nothing else, so "biome" would be ignored:%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate from the "biomes" option: biome source 'minecraft:fixed' is not implemented by this build (SPEC §8)%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate from the "biomes" option: biome source 'minecraft:checkerboard' is not implemented by this build (SPEC §8)%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate from the "biomes" option: biome source 'minecraft:multi_noise' names no parameter list%s
pocketmine\world\generator\InvalidGeneratorOptionsException: Stratum cannot generate from the "biomes" option: invalid character 'M' at position 0 of the namespace in resource location 'Minecraft'%s
