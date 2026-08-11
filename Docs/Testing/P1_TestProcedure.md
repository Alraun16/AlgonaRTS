# P1 Test Procedure

Status: Gates A-D passed in PIE for the previous P1 layers. P1.5 remains implemented and statically validated. The Legacy continuous member-local funnel is preserved; the isolated MassStock local-motion spike awaits the combined runtime gate below. Extreme-choke transit lanes remain deferred.

## Gate A - Group NavMesh + Async Scheduler

Status: core passed in PIE.

Purpose:

```text
MoveGroup
-> group navigation state
-> budgeted async NavMesh request
-> staged completion
-> generation validation
-> compact route
-> route-driven Mass movement
```

Recommended setup:

- Use a map with a valid Recast/NavMesh.
- Keep population moderate for the first Gate A smoke, for example 1k to 5k entities.
- Use more than one group.
- Do not use this gate as a performance benchmark.
- P1 development defaults are applied by code/config. Only enter overrides when a specific scenario calls for them.

Useful commands:

```text
algona.P0.Spawn 1000 100
algona.P0.MoveGroup 1 3000 3000 0
algona.P1.DumpNavigation 1
algona.P0.DumpState 1
```

Burst/stale-result smoke:

```text
algona.P0.MoveGroup 1 3000 3000 0
algona.P0.MoveGroup 1 -3000 3000 0
algona.P0.MoveGroup 1 3000 -3000 0
algona.P1.DumpNavigation 1
```

Expected Gate A evidence:

- `Submitted command` and `Applied command` still use the existing P0 command path.
- `Committed P1 route` appears for current-generation requests.
- `algona.P1.DumpNavigation 1` reports `Status=RouteReady` or a clear failure reason if NavMesh projection/pathing is invalid.
- Projection failure logs include NavSys presence, NavData name/class, start, destination, projection extent, and separate start/destination projection booleans.
- Replaced MoveGroup commands do not commit older-generation paths.
- `QueuedPaths`, `InFlightPaths`, and `PendingPathCompletions` drain after async completion.
- Movement continues through existing Mass integration.

Observed Gate A result:

- Recast/NavMesh projection and async pathfinding worked.
- Debug mouse selection and movement worked; current debug input requires Shift.
- 18 submitted / 18 completed path requests.
- Queues drained to zero.
- Route latency was roughly 15-17 ms in PIE.
- One console destination failed because it was outside the current NavMesh (`DestinationProjected=false`).
- Stale rejection remains implemented but was not forced in this run.

Debug mouse smoke:

```text
algona.P1.DebugMouseInput 1
algona.P1.DebugPickRadius 300
```

Manual actions:

```text
Shift + LMB on/near a presented unit -> selected GroupId log/debug marker
Shift + RMB on ground -> selected group submits MoveGroup through normal command path
```

## Not Covered By Gate A

- formation slots
- local avoidance
- corridor compression
- dynamic blockers
- SimLOD navigation cadence
- multiplayer P1 movement smoke
- formal P1 benchmarks

## Gate B - Formation + Route Corridor Smoke (Historical)

Status: invalid-destination, moving-anchor, and corridor-width probe fixes have runtime smoke evidence. The old global column compression result is superseded by the rework gate below and is not evidence for the new algorithm.

Purpose:

```text
Committed group route
-> formation anchor
-> formation frame
-> stable slots
-> entity slot targets
-> existing Mass movement
```

P1 development defaults are applied by code/config. Gate B should only spawn
the requested population unless a scenario explicitly needs an override.

Useful commands:

```text
algona.P0.Spawn 1000 100
```

Manual smoke:

```text
Shift + LMB on/near a unit
Shift + RMB on a reachable open terrain point
algona.P1.DumpNavigation <SelectedGroupId>
algona.P1.DumpFormation <SelectedGroupId> 8
```

Expected Gate B evidence:

- `Committed P1 route` appears.
- `algona.P1.DumpNavigation` shows `FormationAnchor`, travel/layout/facing vectors, original columns/rows, slots, footprint dimensions, safe anchor offset, corridor summary, settling state, and formation tick.
- `algona.P1.DumpFormation` shows bounded slot target samples with different `Slot=` values and target positions.
- Entities in the selected group move toward distinct moving slot targets around the route look-ahead anchor instead of collapsing onto one distant route point.
- Shift + RMB directly into non-navigable space produces `ProjectionFailed` / `DestinationProjected=false` and does not move the group directly toward that raw target.
- On open terrain, the original formation remains wide instead of becoming a narrow line during ordinary turns.
- On a moderate bridge/corridor approach, local member targets narrow without changing the original `Columns`/`Rows` topology.
- On bridges, `CorridorWidth` should be close to the actual walkable bridge width, and `LeftEdge`/`RightEdge` should satisfy `Anchor - Right * CorridorLeftHalf` and `Anchor + Right * CorridorRightHalf` within debug precision.
- After exiting into a wider area, local member offsets return to their original slots gradually.
- On turns, orientation changes smoothly and edge slots should not cut non-navigable corners.
- After the final route point, `Status=Settling` may appear until members converge to final slots; formation updates should not stop immediately while members are still visibly unsettled.
- No per-entity logs are emitted.

Scenarios to use when Gate B is requested:

- open terrain;
- a turn;
- narrow passage or bridge if available on the current map.

## Formation Choke Rework Gate A-D

Status: implementation complete and statically inspected; build/PIE evidence pending.

This gate validates only the replacement foundation before temporary transit lanes are introduced:

```text
stable original slot topology
-> group corridor profile
-> row-local lateral funnel
-> per-group minimum same-group spacing
-> existing separation and ISM presentation
```

Implemented debug controls:

```text
algona.P1.SetGroupMinSpacing <GroupId> <Spacing>
algona.P0.SetGroupDebugCubeSize <GroupId> <Size>
algona.P1.DumpFormation <GroupId> [MaxSamples=8]
```

Expected A-D evidence:

- open terrain keeps the original wide topology and stable slot ordering;
- a moderate choke produces simultaneous `Funneling`, `InsideChoke`, and/or `ReExpanding` members instead of a full-group long column;
- the rear can remain wide while front members narrow, and exited members restore their original offsets locally;
- groups with different minimum spacing stop compressing at different center distances;
- same-group separation does not push members back toward normal spacing when their distance is at or above the configured minimum;
- group cube-size changes refresh existing ISM instances without Actor-per-unit;
- `DumpFormation` reports original row/column identity, local target/spacing/phase, corridor width, soft overlap, and nearest same-group distance;
- explicit dump visualization shows corridor cross-sections and original-to-local target offsets.

Do not use an extreme choke narrower than a full original row at minimum spacing for this gate. `TransitMembers=0` and `TransitLanes=0` are expected until phase E. A non-zero `SoftOverlapMembers` count identifies a scenario that requires the transit-lane fallback rather than proving A-D failure.

## Formation Geometry / Facing / Footprint Stabilization Gate

Status: the previous runtime gate exposed intermittent axial reversal, stop-and-go
movement, anchor-wide corridor constraints, and premature wall-crossing
re-expansion. Continuous local-funnel stabilization is implemented and
statically inspected; the replacement build/PIE gate is pending. Extreme-choke
transit lanes are not part of this gate.

The debug geometry command is:

```text
algona.P0.SpawnRect <GroupCount> <Columns> <Rows>
```

Runtime acceptance scenarios:

- `SpawnRect 1 16 4` and `SpawnRect 1 4 16` initially produce clean,
  non-sheared rectangles with stable, common instance facing;
- repeated direct 180-degree `FaceMovement` commands always reverse travel and
  facing without rotating the axial slot lattice or remapping members through
  the group center;
- a normal 90-degree turn rotates the rectangular layout smoothly without
  accumulated skew or drift;
- a wide `16 x 4` formation follows a wall corner using the formation-safe
  lateral anchor shift instead of cutting the corner as an independent point;
- a moderate choke progresses continuously while rear, approaching, inside,
  and exited members can use different local widths at the same time;
- approaching members begin lateral convergence from narrower look-ahead
  samples ranked by required lateral speed, and group forward speed reduces
  smoothly when full speed would reach the boundary before that convergence can
  complete;
- members already inside a valid corridor continue forward while soft local
  spacing converges; only a current hard-bound violation can stop route-forward
  movement, with lateral correction still active;
- an individual member remains constrained until its physical longitudinal
  position leaves the choke, preventing premature expansion through walls;
- original rows/columns remain stable and no member crosses non-walkable space.

When diagnostics are needed:

```text
algona.P1.DumpFormation <GroupId> 8
```

Expected compact fields include `FacingMode=FaceMovement`, separate
`TravelDirection`, `LayoutForward`, `LayoutRight`, and `FacingDirection`,
`FrameDot` close to zero, stable `OriginalColumns` / `OriginalRows`, and
`RequiredFootprintHalfWidth` / `SafeAnchorLateralOffset`. During a funnel,
`MemberLongitudinal`, `ConstraintLongitudinal`, `NarrowDistance`,
`RequiredLateralSpeed`, per-member `ForwardSpeedScale`, group
`ForwardSpeedScale`, and `SpeedLimitedMembers` explain the continuous slowdown.
`TransitMembers=0` and `TransitLanes=0` remain expected.

## Gate C - Spatial Grid + Local Separation Smoke

Status: local separation rest stability passed in PIE after the minimum-distance separation fix. Full 1k/5k/10k scenario benchmark remains deferred.

Purpose:

```text
Formation slot target
-> uniform spatial grid
-> bounded neighbor query
-> local separation
-> existing Mass movement integration
```

P1 development defaults are applied by code/config. Gate C should only set
scenario-specific population or temporary overrides.

Useful commands:

```text
algona.P0.Spawn 1000 100
```

Manual smoke:

```text
Shift + LMB on/near a unit
Shift + RMB on a reachable point that causes dense movement or crossing
```

Diagnostics only when needed:

```text
algona.P0.DumpState <SelectedGroupId>
algona.P1.DumpNavigation <SelectedGroupId>
```

Expected Gate C evidence:

- `GridEntities` matches the spawned entity count.
- `GridCells` is non-zero.
- `NeighborQueries` is non-zero while entities move.
- `NeighborAvg` and `NeighborMax` remain bounded by `algona.P1.NeighborMaxCandidates`.
- `NeighborMs` and `AvoidanceMs` appear in compact state output.
- `algona.P0.DumpState <SelectedGroupId>` reports rest diagnostics:
  `MaxSlotError`, `AvgSlotError`, `MaxDesiredSpeed`, `AvgDesiredSpeed`,
  `MaxSeparationSpeed`, `AvgSeparationSpeed`, `MovingBySeparation`, and
  `SettledMembers`.
- After a group visually reaches its final formation, `MovingBySeparation`
  should be zero or quickly converge to zero. Stable square formations should
  not keep producing separation velocity solely because members are within the
  neighbor query radius.
- Dense/crossing movement gets local separation response without per-entity logs.
- No Actor-per-unit, per-entity global pathfinding, or global O(N^2) neighbor search is introduced.

Observed Gate C rest-stability result:

- after arrival, stable formations no longer self-repel solely because members
  are inside the neighbor query radius.

## Gate D - Dynamic Blocker + Repath Smoke

Status: runtime verified in PIE. Gate D PASS.

Purpose:

```text
Active group route
-> debug hard blocker insertion
-> route invalidation
-> budgeted repath or safe blocked state
-> movement clamp against blocker
-> blocker removal
-> requeued route
```

P1 development defaults are applied by code/config.

Useful commands:

```text
algona.P0.Spawn 1000 100
```

Manual smoke:

```text
Shift + LMB on/near a unit
Shift + RMB on a reachable point across open terrain
Wait a few seconds on open terrain before adding the blocker.
algona.P1.AddHardBlockerAtGroup <SelectedGroupId> 400
algona.P1.DumpHardBlockers
algona.P1.DumpNavigation <SelectedGroupId>
algona.P0.DumpState <SelectedGroupId>
algona.P1.ClearHardBlockers
```

Use a larger radius only if the current debug map needs it to cover the visible
route/corridor.

Expected Gate D evidence:

- adding a blocker logs `P1 hard blocker added`.
- plain LMB/RMB do not execute P1 debug select/move; Shift is required.
- before adding a blocker, normal open-terrain route following keeps
  `StuckGroups=0` and does not regularly log
  `P1 stuck recovery queued repath`.
- `AddHardBlockerAtGroup` places the blocker ahead of the moving group on its
  current route/formation direction.
- `LastInvalidated` or `TotalRepaths` increases when the blocker intersects an
  active compact route.
- selected group shows `LastRepath=HardBlockerAdded` or
  `LastFailure=RouteBlockedByHardBlocker` if Recast still returns a route
  through the debug blocker.
- entities do not pass through the debug blocker while route recovery is
  pending.
- `BlockedEntities` or `TotalBlockedEntities` becomes non-zero when entities
  attempt to step through the blocker and are clamped.
- clearing blockers logs `P1 hard blockers cleared` and requeues active groups
  that were blocked by the debug layer.
- no per-entity logs are emitted.

This gate validates the debug P1.4 route invalidation/repath boundary. It does
not validate production dynamic Recast geometry or a final blocker policy.

Observed Gate D result:

- modifier-only debug input worked, and plain LMB/RMB did not execute P1 debug
  select/move;
- normal movement did not show repeated false-positive stuck recovery;
- `AddHardBlockerAtGroup` placed the blocker ahead of the moving group;
- the active route was invalidated and recovery went through the existing
  budgeted scheduler;
- routes through the blocker were rejected;
- `TotalBlockedEntities > 0` confirmed that movement clamp executed;
- units visually did not pass through the blocker;
- `ClearHardBlockers` requeued the blocked group and a new route was built;
- no repath storm, Actor-per-unit path, per-entity global pathfinding, or
  per-entity log spam was observed.

## Alternative MassStock Local-Motion Spike

Status: basic MassStock spawn/movement and crash stabilization were runtime
verified previously. Swept bridge-boundary response, removal of group-wide
MassStock slowdown, force diagnostics, and throughput timing are implemented
and statically inspected; the combined bridge gate below is pending.

Select MassStock before creating the population:

```text
algona.P1.LocalMotionMode MassStock
algona.P0.SpawnRect 1 10 5
```

Mode changes do not migrate a live population. Re-run `SpawnRect` after each
mode change. Use Shift+LMB/RMB for the existing debug select/move path.

Visual matrix:

- `10x5` open movement and at least ten 180-degree reversals;
- 90-degree turns with `10x5`, then `5x10`;
- two groups crossing trajectories;
- a wide formation around a wall corner;
- a moderate choke using the existing local funnel targets;
- final anchor/centroid convergence at several destinations.

Compact evidence for the selected group:

```text
algona.P1.DumpLocalMotion <GroupId>
algona.P1.DumpFormation <GroupId> 8
algona.P1.DumpNavigation <GroupId>
```

Expected MassStock diagnostics while moving include non-zero `MoveTargets` and
`AvoidanceEntities`; `AvoidanceEdges` should be non-zero near corridor walls
and may be zero on wide open terrain. `CorridorConstraintEntities` and
`CorridorConstraintCount` may be non-zero at contact. Normal contact is shown
by `NormalMotionClipped`, while `MaxConstraintCorrection` should remain a small
fraction of `AgentRadius` and `MaxPenetrationDepth` should remain near zero.
One dump during active movement arms a one-shot force capture; the following
`P1 local motion force capture` line reports steering, moving-agent avoidance,
and environment residual without enabling per-entity per-tick logs.

For the same `10x5` group, compare an open route with a similar-length route
through the moderate bridge. `RouteLength`, `ActualMoveTime`,
`BridgeTraversalTime`, `AverageForwardSpeed`, `WaitingForFormation`, and
`SettlingTime` separate route throughput from final convergence. Target:

```text
BridgeMoveTime <= 1.5 * OpenTerrainBaselineTime
```

Also perform one wall-grazing move and at least five bridge passes in both
directions. Wall crossing, visible bounce/ejection, multi-second route stalls,
permanent destination drift, repeated whole-formation slot exchange, or a
return to approximately 20-second bridge traversal are failures.

The automated simulation comparison is one command:

```text
algona.P1.RunLocalMotionSpikeSuite 100 64
```

It runs Legacy and MassStock at 100, 1k, and 10k and emits one compact result
per case with fixed-step average/p95/p99, movement/steering/avoidance/boundary
averages, and missed deadlines. It disables group navigation and SimLOD only
inside the suite to isolate local motion, restores prior settings afterward,
and destroys its temporary populations. These timings do not include the
additional duplicate/transient work from eligible auto-registered stock
processors in the global Mass frame pipeline, including obstacle-grid and
smooth-height processing. Observe overall PIE frame CPU separately when
comparing modes. PIE results are a spike baseline, not a formal standalone
performance baseline.

## Gate E - Navigation LOD / SimLOD Smoke

Status: implementation complete; runtime verification pending.

Purpose:

```text
Existing P0 SimLOD fragments
-> derived group navigation tier
-> formation and slot-target cadence
-> movement and local-steering cadence
-> compact workload/timing evidence
```

P1.5 keeps global path requests, hard-blocker invalidation/repath, route
progress, movement clamping, and stuck recovery on their correctness paths.
The single spatial grid still contains the full population; neighbor and
separation queries are decimated by the existing entity SimLOD cadence.

Debug tier override:

```text
algona.P1.SetSimLOD <High|Medium|Low> [GroupId]
```

Omitting `GroupId` applies the tier to the whole authoritative population and
resets the measurement window. `algona.P0.Spawn` also starts a fresh window.

Gate E diagnostics are emitted by:

```text
algona.P0.DumpState <SelectedGroupId>
```

Relevant lines are `P1 SimLOD workload` and `P1 SimLOD measurement`. They show
High/Medium/Low entity/group counts, formation and target updates/skips,
neighbor queries executed/skipped, per-tier executed queries, latest timings,
and average timings for the current measurement window.

Expected evidence:

- High retains full update cadence.
- Medium and Low show non-zero formation-target and neighbor-query LOD skips.
- Low executes less formation/local-steering work than High over comparable
  movement windows.
- Lower-tier groups continue authoritative route progression and arrival.
- Changing tiers during an active route does not lose route, destination,
  command, formation slot mapping, or hard-blocker constraints.
- Returning to High resumes full cadence without teleport, jitter, or state
  corruption.
- No Actor-per-unit or per-entity global pathfinding is introduced.
