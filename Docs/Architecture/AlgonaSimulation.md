# AlgonaSimulation Architecture

Status: P0 implementation complete. Build, PIE lifecycle, command boundary smoke, population suite, 20k simulation/presentation/SimLOD smoke, optional 100k simulation-only ceiling smoke, 1 server / 2 clients multiplayer smoke, and `RunBenchmarkSuite 100 64` PIE benchmark baseline are runtime verified. Formal clean Development Standalone benchmark baseline is deferred.

## Module Boundaries

`AlgonaSimulation` owns the P0 simulation foundation:

- world-scoped simulation lifetime through `UAlgonaSimulationSubsystem`
- fixed-step simulation clock
- Mass entity archetype and fragments
- bulk population/destruction harness
- group registry
- high-level command queue
- straight-line movement processor path
- simulation LOD update stride
- simulation/presentation export boundary

`AlgonaRTS` depends on `AlgonaSimulation` for P0 test actors:

- `AP0TestGameMode`
- `AP0SimulationPlayerController`
- `AP0SimulationCommandBridge`
- `AP0SimulationPresentationActor`

Dependency direction remains:

```text
AlgonaRTS -> AlgonaSimulation -> MassEntity / MassCore
```

## World Lifetime

`UAlgonaSimulationSubsystem` is a `UTickableWorldSubsystem`.

It is created only for:

- `EWorldType::Game`
- `EWorldType::PIE`

Authoritative P0 state is stored on the subsystem instance:

- simulation tick and accumulator
- Mass runtime references for the world-owned `UMassEntitySubsystem`
- spawned Mass entity handles
- group registry
- pending command queue
- benchmark snapshot

No static singleton or module singleton owns runtime simulation state.

## Mass Integration

P0 entities use one stable archetype with these fragments:

- `FAlgonaSimPositionFragment`
- `FAlgonaSimVelocityFragment`
- `FAlgonaSimGroupFragment`
- `FAlgonaSimStateFragment`
- `FAlgonaSimLODFragment`

The authoritative entity state stores position and velocity, not `FTransform`.

`MassCore` is a public dependency because UE 5.8 defines `FMassFragment` in `Mass/EntityFragments.h`.
`MassEntity` remains a private dependency for `FMassEntityManager`, `FMassEntityQuery`, and execution context code.

## Fixed Tick

The subsystem accumulates frame delta time and runs zero or more fixed simulation steps.

Defaults:

- `algona.Simulation.RateHz = 20`
- `algona.Simulation.MaxStepsPerFrame = 4`

`SimulationTick` increments once per fixed step. Excess catch-up time is dropped and logged to avoid runaway recovery.

## Population

`RepopulateSimulationEntities(EntityCount, GroupSize)` uses `FMassEntityManager::BatchCreateEntities`.

Console commands:

- `algona.P0.Spawn <EntityCount> [GroupSize]`
- `algona.P0.Destroy`
- `algona.P0.RunPopulationSuite [GroupSize] [FixedStepsPerSize]`
- `algona.P0.RunCeilingSmoke [EntityCount=100000] [GroupSize] [FixedSteps]`
- `algona.P0.RunBenchmarkSuite [GroupSize] [FixedStepsPerCase]`

Auto-population is opt-in:

- `algona.P0.AutoPopulate`
- `algona.P0.EntityCount`
- `algona.P0.GroupSize`

## Groups And Commands

Group state lives outside per-entity Mass fragments in `FAlgonaSimGroupState`.

Each entity stores only its compact `GroupId`.

External movement requests use a command boundary:

```text
SubmitMoveGroupCommand
  -> PendingCommands
  -> next fixed simulation tick
  -> group state mutation
  -> Mass movement query
```

Console commands:

- `algona.P0.MoveGroup <GroupId> <X> <Y> <Z>`
- `algona.P0.MoveAll <X> <Y> <Z>`
- `algona.P0.DumpState [GroupId]`

`MoveGroup` logs one submit event and one apply event. It does not log per entity.

## Movement And LOD

Movement is intentionally simple:

```text
Position += DirectionToGroupTarget * MovementSpeed * FixedDelta
```

The P0 baseline used no pathfinding, collision, avoidance, NavMesh, world traces, per-entity Actor tick, or UObject lookup in the Mass loop. P1 extends the movement source with group routes, formation slots, and a bounded spatial-grid local separation layer while preserving the same fixed-step integration path.

Simulation LOD uses an update stride in `FAlgonaSimLODFragment`.

Defaults:

- High: every fixed tick
- Medium: every 2 fixed ticks
- Low: every 4 fixed ticks

`algona.P0.LODEnabled` can disable stride skipping.

`FAlgonaSimBenchmarkSnapshot` records fixed-step timing, missed-deadline status, movement visit count, updated entity count, and entities skipped by LOD so smoke and benchmark logs can show whether SimLOD is active without logging per entity.

## Presentation Boundary

Simulation exports positions through `ExportEntityPositions`.

`AP0SimulationPresentationActor` is a prototype-only visualization actor using one `UInstancedStaticMeshComponent`.
It does not create one Actor per Mass entity.

In development/debug PIE, `algona.P0.Spawn` broadcasts a debug spawn event that the `AlgonaRTS` harness uses to ensure one presentation actor exists in the current world. Repeated spawn calls reuse the existing presentation actor. Presentation can also be spawned at GameMode startup with `algona.P0.SpawnPresentation` or manually in the current world with `algona.P0.SpawnPresentationActor`.

`algona.P0.DumpPresentation` reports the current prototype presentation actor's instance count and last export/refresh timings.

The simulation-only path remains available when no presentation actor is spawned.

## Multiplayer Boundary

`AP0SimulationPlayerController` is the P0 client-owned command endpoint.
Client console commands call the local owning controller, which sends server RPCs for high-level movement requests.

`AP0SimulationCommandBridge` is a prototype replicated actor for aggregate debug state only.

It does not replicate Mass entities as Actors. It replicates aggregate server state:

- entity count
- group count
- pending command count
- simulation tick
- last submitted command sequence
- last applied command sequence
- last applied command group id

Clients submit high-level move commands through the owning `AP0SimulationPlayerController` server RPCs. The server applies them to the authoritative subsystem command queue.

Console commands for the replicated command path:

- `algona.P0.NetMoveGroup <GroupId> <X> <Y> <Z>`
- `algona.P0.NetMoveAll <X> <Y> <Z>`
- `algona.P0.NetDumpBridge`

Direct client-local population and move commands reject `NM_Client` worlds. Multiplayer setup should spawn/populate on the server and submit client commands through the owning P0 player controller.

Runtime multiplayer smoke is verified for 1 listen server, 2 clients, and 10k server-side Mass entities.
