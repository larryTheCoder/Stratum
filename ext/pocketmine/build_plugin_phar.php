<?php

declare(strict_types=1);

/*
 * Stratum — packs ext/plugin into a plugin phar, the only plugin form
 * PocketMine-MP 5.44.3 loads without DevTools (it registers
 * PharPluginLoader and ScriptPluginLoader, no folder loader).
 *
 *   php -n -d phar.readonly=0 build_plugin_phar.php <ext/plugin> <out.phar>
 *
 * plugin.yml at the root and src/ beside it, which is where PharPluginLoader
 * looks ("$file/src" under the plugin's src-namespace-prefix). Files go in
 * in sorted order, so two builds of one tree hold the same entries. Built
 * locally, never published: a plugin's release packaging is the owner's
 * decision.
 */

if($argc !== 3){
	fwrite(STDERR, "usage: php -d phar.readonly=0 build_plugin_phar.php <plugin dir> <out.phar>\n");
	exit(2);
}
[, $pluginDir, $out] = $argv;
if(!is_file($pluginDir . "/plugin.yml") || !is_dir($pluginDir . "/src")){
	fwrite(STDERR, "FAIL: " . $pluginDir . " has no plugin.yml and src/\n");
	exit(1);
}

$files = ["plugin.yml" => $pluginDir . "/plugin.yml"];
$iterator = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($pluginDir . "/src", FilesystemIterator::SKIP_DOTS));
foreach($iterator as $file){
	/** @var SplFileInfo $file */
	if($file->isFile() && $file->getExtension() === "php"){
		$files["src/" . substr($file->getPathname(), strlen($pluginDir . "/src/"))] = $file->getPathname();
	}
}
ksort($files);

@unlink($out);
$phar = new Phar($out);
$phar->startBuffering();
$phar->setStub("<?php __HALT_COMPILER();");
foreach($files as $local => $path){
	$phar->addFile($path, $local);
}
$phar->setSignatureAlgorithm(Phar::SHA256);
$phar->stopBuffering();
echo "built ", $out, ": ", implode(", ", array_keys($files)), "\n";
