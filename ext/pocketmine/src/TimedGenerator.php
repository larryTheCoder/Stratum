<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use pocketmine\world\ChunkManager;
use pocketmine\world\generator\Generator;
use Stratum\PMMP\StratumGenerator;

/**
 * The plugin's StratumGenerator, with a probe between the two halves of its
 * generateChunk() (tools/pmmp-task-cost): test tooling, never part of the
 * plugin. PocketMine-MP's own PopulationTask runs it as it runs any
 * generator — through ThreadLocalGeneratorContext — so the task around it is
 * the real one, and only the chunk is split.
 *
 * generateChunk() here does exactly what StratumGenerator::generateChunk()
 * does — encode(), then assemble(), then the ChunkManager's setChunk() — on
 * the generator PocketMine-MP's AsyncGeneratorRegisterTask built, with
 * Probe::now() at the three boundaries. The harness's other sweep runs the
 * real generateChunk() with no probe in it, which is how a probe's cost is
 * seen to be small.
 *
 * In a counting run (PerfControl) the probes give way to perf's switch:
 * the counted segment's events are on for the engine call ("engine"), the
 * PHP half ("plugin"), or everything in the task except this generateChunk
 * ("pmmp", which the harness switches on round the whole task).
 */
final class TimedGenerator extends Generator{

	/** @var array{array{int, int, int}, array{int, int, int}, array{int, int, int}}|null the last chunk's probes: before encode(), after it, after setChunk() */
	public ?array $marks = null;

	/** Whether to keep what encode() returns, for the harness's check (the first pass only). */
	public bool $keepEncoded = false;

	/** @var array<int, mixed>|null what encode() returned for the last chunk, when kept */
	public ?array $encoded = null;

	/** Whether this chunk is counted (a counting run's timed pass): perf's switch instead of the probes. */
	public bool $counting = false;

	public function __construct(
		private StratumGenerator $inner,
		private ?PerfControl $control = null,
		private string $counted = ""
	){
		// Generator's own seed and preset are its subclasses' to read; this
		// one reads neither, and never draws from $this->random.
		parent::__construct(0, "");
	}

	public function generateChunk(ChunkManager $world, int $chunkX, int $chunkZ) : void{
		if($this->counting && $this->control !== null){
			$this->switchAt($this->control, "engine", "pmmp");
			$encoded = $this->inner->encode($chunkX, $chunkZ);
			$this->switchAt($this->control, "plugin", "engine");
			$world->setChunk($chunkX, $chunkZ, $this->inner->assemble($encoded));
			$this->switchAt($this->control, "pmmp", "plugin");
		}else{
			$before = Probe::now();
			$encoded = $this->inner->encode($chunkX, $chunkZ);
			$encodedAt = Probe::now();
			$world->setChunk($chunkX, $chunkZ, $this->inner->assemble($encoded));
			$this->marks = [$before, $encodedAt, Probe::now()];
		}
		if($this->keepEncoded){
			$this->encoded = $encoded;
		}
	}

	/**
	 * A boundary in a counting run: perf's events go on here when the
	 * counted segment is $starts, off when it is $ends.
	 */
	private function switchAt(PerfControl $control, string $starts, string $ends) : void{
		if($this->counted === $starts){
			$control->enable();
		}elseif($this->counted === $ends){
			$control->disable();
		}
	}

	public function populateChunk(ChunkManager $world, int $chunkX, int $chunkZ) : void{
		$this->inner->populateChunk($world, $chunkX, $chunkZ);
	}
}
