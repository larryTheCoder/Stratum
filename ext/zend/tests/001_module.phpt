--TEST--
The module loads, and the block state table answers with typed Bedrock states
--EXTENSIONS--
stratum
--FILE--
<?php
var_dump(Stratum\javaBlockStateCount());
var_dump(Stratum\bedrockBlockState(0));
// minecraft:snow[layers=1]: a byte and an int, which are different states.
var_dump(Stratum\bedrockBlockState(6718));
foreach([-1, Stratum\javaBlockStateCount()] as $bad){
	try{
		Stratum\bedrockBlockState($bad);
	}catch(Stratum\GenerationException $e){
		echo get_class($e), " is a RuntimeException: ", var_export($e instanceof RuntimeException, true), "\n";
	}
}
try{
	new Stratum\Dimension();
}catch(Error $e){
	echo "Dimension has no public constructor\n";
}
?>
--EXPECT--
int(29671)
array(3) {
  ["name"]=>
  string(13) "minecraft:air"
  ["states"]=>
  array(0) {
  }
  ["version"]=>
  int(18168865)
}
array(3) {
  ["name"]=>
  string(20) "minecraft:snow_layer"
  ["states"]=>
  array(2) {
    ["covered_bit"]=>
    array(2) {
      [0]=>
      int(1)
      [1]=>
      int(0)
    }
    ["height"]=>
    array(2) {
      [0]=>
      int(3)
      [1]=>
      int(0)
    }
  }
  ["version"]=>
  int(18168865)
}
Stratum\GenerationException is a RuntimeException: true
Stratum\GenerationException is a RuntimeException: true
Dimension has no public constructor
