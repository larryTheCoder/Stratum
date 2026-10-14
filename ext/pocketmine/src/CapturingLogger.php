<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use function get_class;

/**
 * A \Logger that keeps every record instead of printing it, so a check can
 * see exactly what the plugin logged — the BlockStateTranslator says each
 * fallback through \GlobalLogger, once per Java state.
 */
final class CapturingLogger extends \SimpleLogger{

	/** @var list<array{string, string}> [level, message] */
	public array $records = [];

	public function log($level, $message){
		$this->records[] = [(string) $level, (string) $message];
	}

	public function logException(\Throwable $e, $trace = null){
		$this->records[] = [\LogLevel::CRITICAL, get_class($e) . ": " . $e->getMessage()];
	}
}
