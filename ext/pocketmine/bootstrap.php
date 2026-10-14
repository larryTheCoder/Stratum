<?php

declare(strict_types=1);

/*
 * Stratum — PocketMine-MP 5.44.3's own classes, without booting a server.
 *
 * Every check in this directory starts here. tools/pmmp-dev runs them under
 * pmmp/PHP-Binaries' own php, with stratum.so loaded beside the extensions
 * PocketMine-MP needs, so what answers each question is PocketMine-MP
 * itself: its Composer autoloader, read straight out of the release phar
 * (which also defines its data paths, pocketmine\BEDROCK_DATA_PATH and the
 * rest), and its own ThreadSafeClassLoader for the plugin's classes and this
 * directory's — the loader a pmmpthread thread registers, so a thread can
 * load them too. No Server is constructed.
 */

namespace Stratum\PocketMineEvidence;

use pocketmine\thread\ThreadSafeClassLoader;
use function array_diff;
use function array_keys;
use function array_slice;
use function count;
use function dirname;
use function extension_loaded;
use function fwrite;
use function implode;
use function is_file;
use function str_starts_with;
use function strpos;
use function substr;
use const PHP_EOL;
use const STDERR;

/**
 * Says what is wrong, on stderr, and exits 1: a failed check.
 */
function fail(string $message) : never{
	fwrite(STDERR, "FAIL: " . $message . PHP_EOL);
	exit(1);
}

/**
 * The script's --name=value arguments. Every name in $required must be
 * given, and nothing else may be: a misspelt option is refused, not ignored.
 *
 * @param list<string> $argv
 * @param list<string> $required
 * @param list<string> $optional
 *
 * @return array<string, string>
 */
function arguments(array $argv, array $required, array $optional = []) : array{
	$given = [];
	foreach(array_slice($argv, 1) as $argument){
		$equals = strpos($argument, "=");
		if(!str_starts_with($argument, "--") || $equals === false){
			fail("expected --name=value, got " . $argument);
		}
		$given[substr($argument, 2, $equals - 2)] = substr($argument, $equals + 1);
	}
	$missing = array_diff($required, array_keys($given));
	if(count($missing) > 0){
		fail("missing --" . implode(", --", $missing));
	}
	$unknown = array_diff(array_keys($given), $required, $optional);
	if(count($unknown) > 0){
		fail("unknown --" . implode(", --", $unknown));
	}
	return $given;
}

/**
 * Loads PocketMine-MP's autoloader from $phar and registers a
 * ThreadSafeClassLoader for the plugin (ext/plugin/src) and this directory's
 * classes (src/), which is returned so threads can be handed it.
 */
function bootstrap(string $phar) : ThreadSafeClassLoader{
	if(!extension_loaded("stratum")){
		fail("stratum.so is not loaded into this php");
	}
	if(!is_file($phar)){
		fail("no PocketMine-MP phar at " . $phar);
	}
	require "phar://" . $phar . "/vendor/autoload.php";

	$loader = new ThreadSafeClassLoader();
	$loader->addPath("Stratum\\PMMP", dirname(__DIR__) . "/plugin/src");
	$loader->addPath("Stratum\\PocketMineEvidence", __DIR__ . "/src");
	$loader->register(false);
	return $loader;
}
