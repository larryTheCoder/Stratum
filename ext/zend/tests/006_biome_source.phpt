--TEST--
parseBiomeSource reads a biome source as Dimension::open will, and refuses what it would, with its reason
--EXTENSIONS--
stratum
--FILE--
<?php
// No pipeline is opened: this is world::BiomeSource::parse alone, which is
// what lets the plugin refuse a source on the main thread.
foreach(["minecraft:the_end", "the_end", "overworld", "minecraft:nether", "stratum:fixed"] as $good){
	echo $good, " -> ", Stratum\parseBiomeSource($good), "\n";
}
foreach(["minecraft:fixed", "checkerboard", "minecraft:multi_noise", "minecraft:the:end", "Minecraft:The_End", ""] as $bad){
	try{
		Stratum\parseBiomeSource($bad);
		echo $bad, ": no exception\n";
	}catch(Stratum\GenerationException $e){
		echo $bad, ": ", $e->getMessage(), "\n";
	}
}
?>
--EXPECTF--
minecraft:the_end -> minecraft:the_end
the_end -> minecraft:the_end
overworld -> minecraft:overworld
minecraft:nether -> minecraft:nether
stratum:fixed -> stratum:fixed
minecraft:fixed: biome source 'minecraft:fixed' is not implemented by this build (SPEC §8): only minecraft:multi_noise, named by its parameter list, and minecraft:the_end are
checkerboard: biome source 'minecraft:checkerboard' is not implemented by this build (SPEC §8): only minecraft:multi_noise, named by its parameter list, and minecraft:the_end are
minecraft:multi_noise: biome source 'minecraft:multi_noise' names no parameter list: name the list it searches instead (minecraft:overworld, minecraft:nether)
minecraft:the:end: resource location 'minecraft:the:end' has more than one ':'
Minecraft:The_End: invalid character 'M' at position 0 of the namespace in resource location 'Minecraft'
: resource location 'minecraft:' has an empty path
