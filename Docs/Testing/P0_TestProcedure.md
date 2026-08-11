# P0 Test Procedure

Status: P0 procedure complete. Build, PIE lifecycle, command boundary smoke, population suite, 20k simulation/presentation/SimLOD smoke, optional 100k simulation-only ceiling smoke, multiplayer smoke, and `RunBenchmarkSuite 100 64` PIE benchmark baseline are verified. Formal clean Development Standalone benchmark baseline is deferred.

## Build Gate

Run:

```text
"F:\UE_5.8\Engine\Build\BatchFiles\Build.bat" AlgonaRTSEditor Win64 Development -Project="F:\AlgonaRTS\AlgonaRTS.uproject" -WaitMutex
```

Expected:

- `AlgonaRTS` and `AlgonaSimulation` compile.
- No UHT errors for P0 `USTRUCT`, subsystem, RPC, GameMode, or actor classes.
- No abstract `UTickableWorldSubsystem` error.

Status: passed.

## Automation Smoke

After a successful build, run the automation test:

```text
"F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "F:\AlgonaRTS\AlgonaRTS.uproject" -ExecCmds="Automation RunTests Algona.P0.Simulation.Types; Quit" -unattended -nop4
```

Expected:

- `Algona.P0.Simulation.Types` passes.

## PIE Lifecycle

In Unreal Editor:

1. Open any lightweight test map.
2. Set GameMode override to `/Script/AlgonaRTS.P0TestGameMode`, or launch with an equivalent `?game=/Script/AlgonaRTS.P0TestGameMode` option.
3. Start PIE.
4. Stop PIE.
5. Start PIE again.

Expected logs:

- `LogAlgonaSimulation: Initialized`
- `LogAlgonaSimulation: Deinitializing`
- no duplicate subsystem crash
- no stale Mass state crash

## Simulation-Only Population Suite

In PIE or standalone console, run the compact population suite:

```text
algona.P0.RunPopulationSuite 100 8
```

Expected:

- one compact `Population suite result` line for each size: 1000, 5000, 10000, 20000
- each line includes entity count, group count, submitted command count, spawn ms, memory observation, movement avg/p95/p99 ms, entity visits, LOD-updated count, and LOD-skipped count
- no Actor-per-unit creation
- no crash across destroy/respawn inside the suite

Manual one-off commands remain available:

```text
algona.P0.Spawn <EntityCount> [GroupSize]
algona.P0.Destroy
```

Status: 10k spawn/cleanup passed. 1k/5k/10k/20k population suite passed.

## Movement And Command Boundary

After spawning:

```text
algona.P0.MoveGroup 1 10000 0 0
algona.P0.MoveAll 0 10000 0
algona.P0.DumpState 1
```

Expected:

- commands are accepted only for existing groups
- command application happens on fixed simulation tick
- `Submitted command` and `Applied command` appear once per group command, not once per entity
- `P0 group state` shows the selected group's active command state
- `SimulationTick` increases monotonically
- movement scope appears in Insights when tracing

Status: `MoveGroup` submit/apply and `DumpState` passed for group 1.

## Simulation LOD

Run with default:

```text
algona.P0.LODEnabled 1
```

Then compare with:

```text
algona.P0.LODEnabled 0
```

Expected:

- movement remains functional in both modes
- LOD enabled mode uses update stride for medium/low entities
- `algona.P0.DumpState` or `Population suite result` shows non-zero `SkippedByLOD` when LOD is enabled and movement is active

## Presentation Stress

`algona.P0.Spawn` automatically ensures one prototype ISM presentation actor in development/debug PIE. Use simulation-only commands only when presentation is intentionally not needed.

```text
algona.P0.Spawn 20000 100
algona.P0.MoveAll 10000 10000 0
algona.P0.DumpState 1
algona.P0.DumpPresentation
```

Expected:

- one `AP0SimulationPresentationActor`
- one `UInstancedStaticMeshComponent`
- no 20k replicated or ticking unit Actors
- visible ISM instances update from the simulation export path
- `P0 presentation` includes presented entity count, instance count, last export ms, and last refresh ms

The startup CVar remains available before PIE:

```text
algona.P0.SpawnPresentation 1
```

The manual fallback command also remains available and reuses the existing actor if one already exists:

```text
algona.P0.SpawnPresentationActor
```

Status: 20k simulation + presentation + SimLOD smoke passed.

## Optional Ceiling Smoke

This is not a target benchmark or production scale requirement. It only checks architectural headroom in the simulation-only path.

```text
algona.P0.RunCeilingSmoke 100000 100 8
```

Expected:

- 100k entities spawn server-side in Mass
- one compact `Ceiling smoke result` line includes spawn ms, memory observation, movement avg/p95/p99 ms, and LOD counters
- command diagnostics are not logged per group during this command
- no presentation actor is required for this test

Status: optional 100k simulation-only ceiling smoke passed.

## Multiplayer Smoke

Use `AP0TestGameMode`.

Minimum target:

```text
1 listen server
2 clients
10000 server-side Mass entities
```

Expected:

- server owns `UAlgonaSimulationSubsystem`
- clients use `AP0SimulationPlayerController` as the owned server RPC endpoint
- `AP0SimulationCommandBridge` exists as one replicated command bridge actor
- client command reaches server RPC
- server queues the high-level group command
- clients observe aggregate replicated server state changing
- after a client command, `NetDumpBridge` shows increased `LastSubmitted` and `LastApplied`, with `LastAppliedGroup=1` for the group command
- no replicated Actor per Mass entity

Useful console commands:

```text
algona.P0.Spawn 10000 100
algona.P0.NetDumpBridge
algona.P0.NetMoveGroup 1 12000 0 0
algona.P0.NetMoveAll 12000 12000 0
algona.P0.NetDumpBridge
```

Run `algona.P0.Spawn` on the server world. Run `algona.P0.NetMoveGroup` or `algona.P0.NetMoveAll` from a client console to verify the owning PlayerController RPC path. `algona.P0.NetDumpBridge` reports the aggregate replicated debug state. Direct client-local population and move commands reject `NM_Client` worlds.

Status: 1 listen server + 2 clients passed with 10k server-side Mass entities. Both clients submitted `NetMoveGroup`; the server received and applied both commands; bridge state replicated to both clients; shutdown was clean.

## Formal Benchmark Suite

Run in Development standalone for primary measurements:

```text
algona.P0.RunBenchmarkSuite 100 64
```

Expected:

- one `Benchmark suite result` line for each case/entity-count pair
- entity counts: 1000, 5000, 10000, 20000
- cases: `Idle_MovementDisabled`, `Moving_LODEnabled`, `Moving_LODDisabled`
- each result includes `StepAvgMs`, `StepP95Ms`, `StepP99Ms`, `MoveAvgMs`, `MoveP95Ms`, `MoveP99Ms`, `MissedDeadlines`, memory observation, and LOD counters
- no per-entity logging

Status: runtime verified as a multi-window PIE benchmark baseline with 12 completed cases and `MissedDeadlines=0`. Clean Development Standalone execution is deferred unless formal performance numbers are required.

## Logs To Paste Back On Failure

For build failures:

- first UHT/compiler error block
- the first error mentioning `AlgonaSimulation`, `P0`, `Mass`, `GetStatId`, or RPC

For runtime failures:

- `LogAlgonaSimulation` lines around startup/spawn/move/destroy
- crash assertion text
- PIE or network error lines
