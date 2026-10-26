<?php

declare(strict_types=1);

namespace Stratum\PocketMineEvidence;

use pocketmine\block\Block;
use pocketmine\GarbageCollectorManager;
use pocketmine\thread\Thread;
use pocketmine\world\format\Chunk;
use pocketmine\world\format\io\FastChunkSerializer;
use pocketmine\world\generator\executor\AsyncGeneratorRegisterTask;
use pocketmine\world\generator\executor\GeneratorExecutorSetupParameters;
use pocketmine\world\generator\executor\ThreadLocalGeneratorContext;
use pocketmine\world\generator\PopulationTask;
use pocketmine\world\World;
use Stratum\PMMP\BlockStateTranslator;
use Stratum\PMMP\StratumGenerator;
use function array_keys;
use function array_map;
use function count;
use function json_encode;
use function sort;
use function str_starts_with;
use const JSON_THROW_ON_ERROR;

/**
 * One PocketMine-MP generation worker's share of tools/pmmp-task-cost: a
 * pocketmine\thread\Thread that does with a world's chunks what an
 * AsyncWorker does with its population tasks, one chunk at a time, and
 * times each part (ext/pocketmine/task_cost.php says what is timed and what
 * is left out).
 *
 * The generator is built by PocketMine-MP's own AsyncGeneratorRegisterTask
 * from GeneratorExecutorSetupParameters — the plugin's StratumGenerator,
 * constructed with the world's seed and options string, as on a worker —
 * and found through ThreadLocalGeneratorContext, as PopulationTask finds
 * it. Each chunk is then PocketMine-MP's own PopulationTask, constructed as
 * the main thread constructs it for a chunk not yet generated, with no
 * adjacent chunks given, so that it generates that one chunk and no other.
 * Its onRun() is the worker's side: the generator's generateChunk() and
 * populateChunk(), then FastChunkSerializer's serialisation of the result
 * into the task. GarbageCollectorManager::maybeCollectCycles() follows, as
 * AsyncTask::run() calls it after every task on a worker. onCompletion()
 * is the main thread's side, which deserialises the chunk back.
 *
 * Two sweeps a pass, chunk by chunk, as the bench runs its stages and then
 * its end to end: the split sweep runs each task over TimedGenerator, which
 * times StratumGenerator's encode() (the zend call) and assemble() (the
 * plugin's PHP) apart; the end-to-end sweep runs it over the StratumGenerator
 * itself, untouched. On the first pass every chunk of both sweeps is held
 * to the bench's dump (ChunkDump): what encode() returned, the chunk
 * onCompletion() handed back, and the fluid updates left in the extension's
 * outbox.
 */
final class TaskCostThread extends Thread{

	/** ThreadLocalGeneratorContext ids, as AsyncGeneratorExecutor numbers its worlds. */
	private const CONTEXT_REAL = 1;
	private const CONTEXT_TIMED = 2;

	/** How many differences a check lists before it only counts them. */
	private const LISTED = 8;

	/** How many back-to-back probes measure a probe's own cost. */
	private const PROBE_CALIBRATION = 2000;

	/**
	 * The segments a counting run can count, and the sweep each is in.
	 * "window" is an empty one, perf switched on and straight off again: what
	 * a counting window costs with nothing in it, which the instrument takes
	 * off every other segment's count.
	 */
	public const COUNTED = [
		"engine" => "split", "plugin" => "split", "pmmp" => "split", "serialize" => "split",
		"task" => "e2e", "gc" => "e2e", "worker" => "e2e", "completion" => "e2e", "window" => "e2e"
	];

	/** JSON: what onRun() measured and checked; "" until it has finished. */
	public string $result = "";

	/**
	 * @param string $counted "" to time; else a key of COUNTED, counted by
	 * perf through $controlFifo and $ackFifo
	 */
	public function __construct(
		private string $blob,
		private string $noiseSettings,
		private string $biomeSource,
		private int $seed,
		private string $generatorOptions,
		private string $dumpPath,
		private int $warmup,
		private int $repeat,
		private string $counted = "",
		private string $controlFifo = "",
		private string $ackFifo = ""
	){}

	protected function onRun() : void{
		$logger = new CapturingLogger();
		\GlobalLogger::set($logger);
		$dump = new ChunkDump($this->dumpPath);
		if($dump->seed !== $this->seed){
			fail($this->dumpPath . " is the bench's dump for seed " . $dump->seed . ", not " . $this->seed);
		}
		$control = $this->counted !== "" ? new PerfControl($this->controlFifo, $this->ackFifo) : null;
		// An AsyncWorker's own collector: PHP's automatic one is off on a
		// worker, and AsyncTask::run() collects after each task once enough
		// roots wait.
		$collector = new GarbageCollectorManager($logger, null);

		// The worker's setup, as the first population task a worker is given
		// for a world brings it: AsyncGeneratorRegisterTask, run here inline.
		$setupStart = Probe::now();
		(new AsyncGeneratorRegisterTask(
			new GeneratorExecutorSetupParameters(World::Y_MIN, World::Y_MAX, $this->seed, StratumGenerator::class, $this->generatorOptions),
			self::CONTEXT_REAL
		))->onRun();
		$setup = Probe::span($setupStart, Probe::now());
		$generator = ThreadLocalGeneratorContext::fetch(self::CONTEXT_REAL)?->getGenerator();
		if(!$generator instanceof StratumGenerator){
			fail("AsyncGeneratorRegisterTask did not register a StratumGenerator");
		}
		$timed = new TimedGenerator($generator, $control, $this->counted);
		ThreadLocalGeneratorContext::register(new ThreadLocalGeneratorContext($timed, World::Y_MIN, World::Y_MAX), self::CONTEXT_TIMED);

		// The check's own translator: what the plugin's should have put in
		// each palette, from the same PocketMine-MP upgrader and deserializer.
		$reference = new BlockStateTranslator();

		$probe = self::probeCost();
		$samples = [];
		$checks = ["chunks" => 0, "sub_chunks" => 0, "fluid_updates" => 0, "differences" => 0, "listed" => []];
		$windows = 0;
		$sweeps = $this->counted === "" ? ["split", "e2e"] : [self::COUNTED[$this->counted]];
		for($pass = 0; $pass < $this->warmup + $this->repeat; $pass++){
			$isTimed = $pass >= $this->warmup;
			// The first pass keeps what encode() returned, for the check.
			$timed->keepEncoded = $pass === 0;
			foreach($sweeps as $sweep){
				foreach($dump->chunks as $i => [$chunkX, $chunkZ]){
					$counted = $control !== null && $isTimed ? $this->counted : "";
					$timed->counting = $counted !== "";
					if($counted !== ""){
						$windows++;
					}
					[$result, $spans] = $this->runTask($sweep, $chunkX, $chunkZ, $timed, $collector, $control, $counted);

					// The main thread's, not the task's: taken outside every span.
					$fluid = \Stratum\takeFluidUpdates($this->blob, $this->noiseSettings, $this->biomeSource, $this->seed, $chunkX, $chunkZ);
					if($pass === 0){
						$this->check($checks, $sweep, $dump->records[$i], $sweep === "split" ? $timed->encoded : null, $result, $fluid, $reference, $chunkX, $chunkZ);
					}
					$timed->encoded = null;
					if($isTimed && $control === null){
						foreach($spans as $segment => $span){
							$samples[$segment][$i][] = $span;
						}
					}
				}
			}
		}
		$control?->close();
		$enables = $control?->enables ?? 0;

		$warnings = [];
		$gcRuns = 0;
		foreach($logger->records as [$level, $message]){
			if($level === \LogLevel::INFO && str_starts_with($message, "[Cyclic Garbage Collector]")){
				$gcRuns++;
			}elseif($level !== \LogLevel::DEBUG && $level !== \LogLevel::INFO){
				$warnings[] = $level . ": " . $message;
			}
		}

		$this->result = json_encode([
			"seed" => $this->seed,
			"dimension" => $dump->dimension,
			"engine_version" => $dump->engineVersion,
			"chunks" => $dump->chunks,
			"setup" => $setup,
			"probe_cost" => $probe,
			"samples" => $samples,
			"counted" => $this->counted,
			"windows" => $windows,
			"enables" => $enables,
			"checks" => $checks,
			"gc_runs" => $gcRuns,
			"warnings" => $warnings
		], JSON_THROW_ON_ERROR);
	}

	/**
	 * One chunk's population task, start to finish: constructed, onRun(),
	 * the worker's collector, onCompletion(). Timed with probes, or, in a
	 * counting run's timed passes, with perf's events on for the counted
	 * segment alone.
	 *
	 * @return array{Chunk, array<string, array{int, int}>} the chunk the main
	 * thread got back, and each segment's [wall ns, CPU ns]
	 */
	private function runTask(string $sweep, int $chunkX, int $chunkZ, TimedGenerator $timed, GarbageCollectorManager $collector, ?PerfControl $control, string $counted) : array{
		$result = null;
		// Constructed as the main thread constructs it, and not timed: for a
		// chunk not yet generated, with no adjacent chunks.
		$task = new PopulationTask(
			$sweep === "split" ? self::CONTEXT_TIMED : self::CONTEXT_REAL,
			$chunkX,
			$chunkZ,
			null,
			[],
			static function(Chunk $centre, array $adjacent) use (&$result) : void{
				$result = $centre;
			}
		);
		$timed->marks = null;

		self::switchAt($control, $counted, ["window"], []);
		self::switchAt($control, $counted, [], ["window"]);
		self::switchAt($control, $counted, ["pmmp", "task", "worker"], []);
		$start = Probe::now();
		$task->onRun();
		$ran = Probe::now();
		self::switchAt($control, $counted, ["gc"], ["pmmp", "task"]);
		$collector->maybeCollectCycles();
		$collected = Probe::now();
		self::switchAt($control, $counted, ["completion"], ["gc", "worker"]);
		$task->onCompletion();
		$completed = Probe::now();
		self::switchAt($control, $counted, [], ["completion"]);
		if(!$result instanceof Chunk){
			fail("PopulationTask handed back no chunk for (" . $chunkX . ", " . $chunkZ . ")");
		}

		if($sweep === "e2e"){
			return [$result, [
				"task" => Probe::span($start, $ran),
				"gc" => Probe::span($ran, $collected),
				"worker" => Probe::span($start, $collected),
				"completion" => Probe::span($collected, $completed)
			]];
		}

		// PocketMine-MP's serialisation of the chunk on its own, on the
		// chunk the main thread got back: the same sub-chunks and layers the
		// worker serialised inside onRun().
		self::switchAt($control, $counted, ["serialize"], []);
		$serializeStart = Probe::now();
		FastChunkSerializer::serializeTerrain($result);
		$serialized = Probe::now();
		self::switchAt($control, $counted, [], ["serialize"]);

		$marks = $timed->marks;
		if($marks === null){
			if($control === null){
				fail("the split sweep's task did not run TimedGenerator for (" . $chunkX . ", " . $chunkZ . ")");
			}
			return [$result, []];
		}
		$task = Probe::span($start, $ran);
		$generate = Probe::span($marks[0], $marks[2]);
		return [$result, [
			"engine" => Probe::span($marks[0], $marks[1]),
			"plugin" => Probe::span($marks[1], $marks[2]),
			"pmmp" => [$task[0] - $generate[0], $task[1] - $generate[1]],
			"split_task" => $task,
			"serialize" => Probe::span($serializeStart, $serialized)
		]];
	}

	/**
	 * A boundary in a counting run: perf's events go on when the counted
	 * segment starts here, off when it ends here.
	 *
	 * @param list<string> $starts
	 * @param list<string> $ends
	 */
	private static function switchAt(?PerfControl $control, string $counted, array $starts, array $ends) : void{
		if($control === null || $counted === ""){
			return;
		}
		foreach($starts as $segment){
			if($segment === $counted){
				$control->enable();
			}
		}
		foreach($ends as $segment){
			if($segment === $counted){
				$control->disable();
			}
		}
	}

	/**
	 * One chunk, held to what the C++ produced for it (the bench's dump):
	 * what encode() returned (the split sweep), identical; the chunk the
	 * main thread got back — every sub-chunk index, the empty block id, each
	 * block layer's width and words identical and its palette the reference
	 * translation of the C++'s, each biome layer identical, the chunk
	 * populated; and the fluid updates left in the outbox, identical and in
	 * order. Each difference is counted, the first few listed.
	 *
	 * @param array<string, mixed> $checks
	 * @param array{subChunks: array<int, array{blocks: array{int, string, list<int>}|null, biomes: array{int, string, list<int>}}>, fluid: list<array{int, int, int}>} $expected
	 * @param array<int, mixed>|null $encoded
	 * @param list<array{int, int, int}> $fluid
	 */
	private function check(array &$checks, string $sweep, array $expected, ?array $encoded, Chunk $chunk, array $fluid, BlockStateTranslator $reference, int $chunkX, int $chunkZ) : void{
		$where = $sweep . " sweep, chunk (" . $chunkX . ", " . $chunkZ . ")";
		$differ = static function(string $what) use (&$checks, $where) : void{
			$checks["differences"]++;
			if(count($checks["listed"]) < self::LISTED){
				$checks["listed"][] = $where . ": " . $what;
			}
		};
		$checks["chunks"]++;
		$indices = array_keys($expected["subChunks"]);
		sort($indices);

		if($sweep === "split"){
			if($encoded === null){
				$differ("encode() returned nothing to check");
			}else{
				$got = array_keys($encoded);
				sort($got);
				if($got !== $indices){
					$differ("encode() returned sub-chunks " . self::list($got) . ", the C++ " . self::list($indices));
				}
				foreach($expected["subChunks"] as $y => $layers){
					if(($encoded[$y] ?? null) !== $layers){
						$differ("encode()'s sub-chunk " . $y . " is not the C++'s");
					}
				}
			}
		}

		$subChunks = $chunk->getSubChunks();
		$got = array_keys($subChunks);
		sort($got);
		if($got !== $indices){
			$differ("the chunk holds sub-chunks " . self::list($got) . ", the C++ " . self::list($indices));
		}
		if(!$chunk->isPopulated()){
			$differ("the chunk came back not populated");
		}
		foreach($expected["subChunks"] as $y => $layers){
			$subChunk = $subChunks[$y] ?? null;
			if($subChunk === null){
				continue;
			}
			$checks["sub_chunks"]++;
			if($subChunk->getEmptyBlockId() !== Block::EMPTY_STATE_ID){
				$differ("sub-chunk " . $y . "'s empty block is " . $subChunk->getEmptyBlockId());
			}
			$blockLayers = $subChunk->getBlockLayers();
			if($layers["blocks"] === null){
				if(count($blockLayers) !== 0){
					$differ("sub-chunk " . $y . " is all air in the C++ and holds " . count($blockLayers) . " block layer(s)");
				}
			}elseif(count($blockLayers) !== 1){
				$differ("sub-chunk " . $y . " holds " . count($blockLayers) . " block layers, not 1");
			}else{
				[$bits, $words, $javaPalette] = $layers["blocks"];
				$layer = $blockLayers[0];
				$palette = array_map(static fn(int $javaStateId) : int => $reference->translate($javaStateId), $javaPalette);
				if($layer->getBitsPerBlock() !== $bits || $layer->getWordArray() !== $words){
					$differ("sub-chunk " . $y . "'s block words are not the C++'s");
				}
				if($layer->getPalette() !== $palette){
					$differ("sub-chunk " . $y . "'s block palette is not the C++'s, translated");
				}
			}
			[$bits, $words, $palette] = $layers["biomes"];
			$biomes = $subChunk->getBiomeArray();
			if($biomes->getBitsPerBlock() !== $bits || $biomes->getWordArray() !== $words || $biomes->getPalette() !== $palette){
				$differ("sub-chunk " . $y . "'s biomes are not the C++'s");
			}
		}

		if($fluid !== $expected["fluid"]){
			$differ(count($fluid) . " fluid update(s) in the outbox, " . count($expected["fluid"]) . " in the C++'s output, or not the same ones in the same order");
		}
		$checks["fluid_updates"] += count($fluid);
	}

	/**
	 * @param list<int> $values
	 */
	private static function list(array $values) : string{
		return count($values) === 0 ? "none" : $values[0] . ".." . $values[count($values) - 1] . " (" . count($values) . ")";
	}

	/**
	 * What a probe itself costs: the mean wall and CPU between back-to-back
	 * probes, as Probe::span() reads them.
	 *
	 * @return array{float, float} [wall ns, CPU ns]
	 */
	private static function probeCost() : array{
		$first = $previous = Probe::now();
		$wall = 0;
		for($i = 0; $i < self::PROBE_CALIBRATION; $i++){
			$next = Probe::now();
			$wall += Probe::span($previous, $next)[0];
			$previous = $next;
		}
		return [$wall / self::PROBE_CALIBRATION, Probe::span($first, $previous)[1] / self::PROBE_CALIBRATION];
	}
}
