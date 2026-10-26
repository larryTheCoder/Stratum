<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use function getrusage;
use function hrtime;

/**
 * The clocks tools/pmmp-task-cost reads at each boundary of a generation
 * task: wall time (hrtime, monotonic, ns) and the process's CPU time
 * (getrusage, user + system, µs).
 *
 * PHP has no per-thread CPU clock, so the CPU figure is the process's —
 * Linux's RUSAGE_SELF, which brings the calling thread's runtime up to the
 * instant it is read, so it is exact to the µs — and the harness keeps the
 * process to one busy thread: the generation thread runs while the thread
 * that started it waits in join(), and nothing else runs PHP. On a quiet
 * machine the two clocks agree; where they do not, the wall figure includes
 * time the thread was not running.
 *
 * A probe reads wall, CPU, wall: the segment that ends at a probe ends at
 * its first wall reading and the one that starts there starts at its last,
 * so no segment's wall time includes a getrusage() call. Its CPU time does —
 * about one call's worth (the harness measures that and reports it as the
 * probe cost).
 */
final class Probe{

	private function __construct(){}

	/**
	 * @return array{int, int, int} [wall ns before, CPU µs, wall ns after]
	 */
	public static function now() : array{
		$before = hrtime(true);
		$usage = getrusage();
		$after = hrtime(true);
		return [
			$before,
			$usage["ru_utime.tv_sec"] * 1_000_000 + $usage["ru_utime.tv_usec"] + $usage["ru_stime.tv_sec"] * 1_000_000 + $usage["ru_stime.tv_usec"],
			$after
		];
	}

	/**
	 * The span from probe $from to probe $to.
	 *
	 * @param array{int, int, int} $from
	 * @param array{int, int, int} $to
	 *
	 * @return array{int, int} [wall ns, CPU ns]
	 */
	public static function span(array $from, array $to) : array{
		return [$to[0] - $from[2], ($to[1] - $from[1]) * 1000];
	}
}
