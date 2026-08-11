# P0 Mass Benchmark

Status: benchmark harness complete for P0. 1k/5k/10k/20k population suite, 20k simulation/presentation/SimLOD smoke, optional 100k simulation-only ceiling smoke, multiplayer smoke, and `RunBenchmarkSuite 100 64` PIE benchmark baseline were runtime verified. Formal clean Development Standalone measurements are deferred.

## Implemented Harness

Simulation-only population and movement can be driven with:

- `algona.P0.Spawn <EntityCount> [GroupSize]`
- `algona.P0.Destroy`
- `algona.P0.MoveAll <X> <Y> <Z>`
- `algona.P0.MoveGroup <GroupId> <X> <Y> <Z>`
- `algona.P0.DumpState [GroupId]`
- `algona.P0.RunPopulationSuite [GroupSize] [FixedStepsPerSize]`
- `algona.P0.RunCeilingSmoke [EntityCount=100000] [GroupSize] [FixedSteps]`
- `algona.P0.RunBenchmarkSuite [GroupSize] [FixedStepsPerCase]`
- `algona.P0.SpawnPresentationActor` (manual fallback; `algona.P0.Spawn` ensures one debug presentation actor in development PIE)
- `algona.P0.DumpPresentation`

Relevant CVars:

- `algona.Simulation.RateHz`
- `algona.Simulation.MaxStepsPerFrame`
- `algona.P0.EntityCount`
- `algona.P0.GroupSize`
- `algona.P0.AutoPopulate`
- `algona.P0.MovementSpeed`
- `algona.P0.MovementEnabled`
- `algona.P0.LODEnabled`
- `algona.P0.SpawnPresentation`

Multiplayer smoke helpers:

- `algona.P0.NetMoveGroup <GroupId> <X> <Y> <Z>`
- `algona.P0.NetMoveAll <X> <Y> <Z>`
- `algona.P0.NetDumpBridge`

`NetMoveGroup` and `NetMoveAll` route through the owning `AP0SimulationPlayerController` server RPCs. `NetDumpBridge` reports aggregate debug state from `AP0SimulationCommandBridge`.
The bridge includes last submitted/applied command sequence markers so command delivery can be confirmed even if the pending queue drains before the next replication update.

`UAlgonaSimulationSubsystem::GetBenchmarkSnapshot()` tracks:

- entity count
- group count
- simulation tick
- last spawn milliseconds
- last destroy milliseconds
- last spawn/destroy used physical memory delta
- last used physical memory
- last fixed-step milliseconds
- last fixed-step missed deadline status
- last movement milliseconds
- last movement entity visits
- last movement updated entities
- last movement entities skipped by LOD
- pending command count

Unreal Insights scopes added:

- `AlgonaSimulation_Tick`
- `AlgonaSimulation_FixedStep`
- `AlgonaSimulation_CommandProcessing`
- `AlgonaSimulation_MassMovement`
- `AlgonaSimulation_EntitySpawn`
- `AlgonaSimulation_EntityDestroy`
- `AlgonaSimulation_PresentationExport`
- `AlgonaSimulation_PresentationUpdate`

`AP0SimulationPresentationActor` also reports compact presentation state through `algona.P0.DumpPresentation`:

- presented entity count
- ISM instance count
- last simulation export milliseconds
- last presentation refresh milliseconds

## Required Runs

Run in Development Standalone for primary measurements when a formal performance baseline is required.

The compact formal simulation-only benchmark suite runs the required small populations automatically:

```text
algona.P0.RunBenchmarkSuite 100 64
```

It emits `Benchmark suite result` for:

- `Idle_MovementDisabled`
- `Moving_LODEnabled`
- `Moving_LODDisabled`

The compact smoke suite remains available:

```text
algona.P0.RunPopulationSuite 100 8
```

Target populations covered by that suite:

- 1k
- 5k
- 10k
- 20k

Optional simulation-only ceiling smoke:

```text
algona.P0.RunCeilingSmoke 100000 100 8
```

Configurations:

```text
A. idle, simulation only: covered by `Idle_MovementDisabled`
B. all moving, simulation only: covered by `Moving_LODDisabled`
C. grouped movement: covered by `Moving_LODEnabled` and `Moving_LODDisabled`
D. mixed simulation LOD: covered by `Moving_LODEnabled`
E. simulation only: covered by `RunBenchmarkSuite`
F. simulation + ISM visualization: smoke verified through presentation actor and `DumpPresentation`; formal presentation timings remain manual/Insights-assisted
```

## Results

Formal clean Development Standalone timings are not recorded.

PIE runtime benchmark baseline:

- `algona.P0.RunBenchmarkSuite 100 64` completed successfully.
- The run was performed in multi-window PIE after multiplayer configuration, not in clean Development Standalone.
- 12 cases completed: `Idle_MovementDisabled`, `Moving_LODEnabled`, `Moving_LODDisabled` x 1k/5k/10k/20k.
- `MissedDeadlines=0` was reported.
- `SpawnMemDeltaKB` and `UsedPhysicalMB` appeared in runtime logs.

Memory fields are runtime observations only. Do not use them as reliable bytes/entity measurements because allocator reuse and editor state can dominate deltas.

Runtime smoke verified:

- 1k/5k/10k/20k compact population suite succeeded.
- 10k Mass entity spawn succeeded.
- 10k cleanup succeeded.
- 20k simulation smoke succeeded.
- 20k presentation + SimLOD smoke succeeded.
- `MoveGroup` submit/apply command path succeeded.
- Optional 100k simulation-only ceiling smoke succeeded.
- Multiplayer 1 listen server + 2 clients smoke succeeded with 10k server-side Mass entities.
- `RunBenchmarkSuite 100 64` PIE benchmark baseline succeeded.

| Entities | Mode | Avg Step ms | p95 | p99 | Spawn ms | Destroy ms | Presentation ms | Notes |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1k | PIE benchmark baseline | observed in log | observed in log | observed in log | observed in log | observed in log | not measured by suite | `RunBenchmarkSuite`; not standalone |
| 5k | PIE benchmark baseline | observed in log | observed in log | observed in log | observed in log | observed in log | not measured by suite | `RunBenchmarkSuite`; not standalone |
| 10k | PIE benchmark baseline | observed in log | observed in log | observed in log | observed in log | observed in log | not measured by suite | `RunBenchmarkSuite`; command smoke passed |
| 20k | PIE benchmark baseline | observed in log | observed in log | observed in log | observed in log | observed in log | observed in PIE | Simulation/presentation/SimLOD smoke passed |
| 100k | ceiling smoke passed | not benchmarked | not benchmarked | not benchmarked | observed in log | observed in log | n/a | `RunCeilingSmoke`; simulation-only headroom check, not target benchmark |

## Measurement Notes

Do not treat FPS as proof of simulation scalability.

Record for each run:

- UE version
- build configuration
- CPU/GPU/RAM
- entity count
- simulation Hz
- group size
- LOD settings
- presentation enabled/disabled
- average, p95, p99 simulation step cost
- spawn and destroy cost
- memory observations
- network observations for multiplayer runs

## Current Bottlenecks

No measured bottlenecks requiring P0 redesign were identified. The PIE benchmark baseline is not a clean standalone performance baseline.

Deferred technical debt:

- Record a clean Development Standalone benchmark baseline.
- Capture hardware/OS/build configuration alongside final numbers.
- Treat memory fields as allocator/editor-state observations, not bytes/entity.
