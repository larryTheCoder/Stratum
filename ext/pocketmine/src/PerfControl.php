<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use function fclose;
use function fflush;
use function fgets;
use function fopen;
use function fwrite;
use function trim;

/**
 * `perf stat` / `perf record --control fifo:CTL,ACK -D -1`, from inside the
 * process it counts: events start disabled, and are counted only between
 * enable() and disable(), so a run counts one segment of the generation
 * task and nothing else — not PHP's start, not the warm-up pass, not the
 * other segments (tools/pmmp-task-cost). Each call waits for perf's ack,
 * so the counters are on before the segment starts and off before the next
 * one does.
 */
final class PerfControl{

	/** @var resource */
	private $control;

	/** @var resource */
	private $ack;

	private bool $enabled = false;

	/** How many times the events went on: each costs a window's overhead, which the instrument calibrates. */
	public int $enables = 0;

	public function __construct(string $controlFifo, string $ackFifo){
		$control = fopen($controlFifo, "w");
		$ack = fopen($ackFifo, "r");
		if($control === false || $ack === false){
			fail("cannot open perf's control fifos " . $controlFifo . " and " . $ackFifo);
		}
		$this->control = $control;
		$this->ack = $ack;
	}

	public function enable() : void{
		$this->send("enable");
		$this->enabled = true;
		$this->enables++;
	}

	public function disable() : void{
		$this->send("disable");
		$this->enabled = false;
	}

	public function close() : void{
		if($this->enabled){
			$this->disable();
		}
		fclose($this->control);
		fclose($this->ack);
	}

	private function send(string $command) : void{
		if(fwrite($this->control, $command . "\n") === false || !fflush($this->control)){
			fail("perf's control fifo refused '" . $command . "'");
		}
		// perf writes "ack\n" with its terminating NUL, which the next read
		// then starts with.
		$answer = fgets($this->ack);
		if($answer === false || trim($answer, "\n\0") !== "ack"){
			fail("perf did not acknowledge '" . $command . "'");
		}
	}
}
