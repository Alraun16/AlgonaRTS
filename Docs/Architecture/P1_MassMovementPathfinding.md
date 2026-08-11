# P1 Mass Movement / Pathfinding

Status: P1.0-P1.4 runtime gates passed. P1.5 Navigation LOD / SimLOD integration remains implemented and statically validated. The continuous member-local funnel remains the Legacy path; an isolated stock Mass local-motion alternative is implemented and awaits runtime comparison. Extreme-choke transit lanes remain deferred.

## Implemented Vertical Slice

The existing `UAlgonaSimulationSubsystem` remains the world-scoped owner. P1 does not add a second simulation clock, command queue, group model, or subsystem.

Implemented pipeline:

```text
Existing MoveGroup command
-> existing fixed-step command queue
-> group navigation generation update
-> budgeted async NavMesh scheduler
-> completion staging
-> fixed-step generation validation
-> compact group route points
-> route progression
-> formation anchor / stable slots
-> group-level longitudinal corridor profile
-> per-member local funnel / re-expansion
-> local steering with same-group minimum spacing
-> existing Mass movement integration
```

The route state is group-owned through `FAlgonaGroupNavigationState`. Mass entities do not own global NavMesh paths.

## Scheduler

Console variables:

- `algona.P1.NavigationEnabled`: enables P1 route targets. When enabled, failed or pending P1 routes do not fall back to raw P0 direct movement toward the command target.
- `algona.P1.PathSubmitBudget`: maximum async path requests submitted per fixed step.
- `algona.P1.RouteAcceptanceRadius`: group-center threshold for route point progression.
- `algona.P1.NavProjectionExtent`: XY projection extent for route endpoints.
- `algona.P1.NavProjectionExtentZ`: Z projection extent for route endpoints. This defaults wider than XY so console targets with `Z=0` can still project onto real terrain/NavMesh height.

Async UE API used:

- `UNavigationSystemV1::FindPathAsync`
- `UNavigationSystemV1::AbortAsyncFindPathRequest`

Completed UE paths are converted immediately into compact `TArray<FVector>` route points before fixed-step commit. Heavy UE path objects are not kept in hot group state.

## Generation Safety

Each Move command increments `RouteGeneration`. Queued and in-flight path requests carry that generation. A completion commits only when:

```text
Completion.Generation == Group.Navigation.RouteGeneration
Completion.RequestId == Group.Navigation.PendingRequestId
```

Otherwise the completion is counted as stale and discarded.

## Diagnostics

Existing `algona.P0.DumpState` now includes P1 scheduler counters:

- queued path requests
- in-flight path requests
- pending completions
- submitted/completed/failed/stale path totals
- scheduler milliseconds
- route progression milliseconds

New command:

```text
algona.P1.DumpNavigation [GroupId]
algona.P1.DumpFormation [GroupId] [MaxSamples=8]
```

It prints one group route status, generation, request id, route point count, current route point, current movement target, partial flag, valid flag, commit tick, and last failure reason.

`DumpFormation` prints one group formation summary plus a bounded sample of slot targets and desired velocities.

When a compact route exists, the same command draws temporary debug lines for that group's route.

Projection failures include compact diagnostics for:

- whether `UNavigationSystemV1` exists;
- selected `ANavigationData` name/class;
- start and destination;
- projection extent;
- separate start and destination projection results.

Gate A runtime result:

- Recast/NavMesh projection and async pathfinding worked for mouse-selected destinations.
- Debug mouse command path worked through the existing server-authoritative command boundary; current debug input requires Shift + mouse.
- 18 path requests were submitted and 18 completed.
- Path queues drained to zero.
- Observed route latency was roughly 15-17 ms in PIE.
- One console destination failed because the clicked/typed target was outside the current NavMesh (`DestinationProjected=false`).
- Stale rejection remains implemented but was not artificially forced in this run.

## Formation Vertical Slice

P1.2 adds a minimal group formation layer:

- `FAlgonaFormationState` on each group;
- `FAlgonaSimNavigationFragment` on each Mass entity;
- stable `SlotIndex` assigned during population;
- route-driven formation anchor;
- separate travel, layout, and facing directions;
- an orthonormal `LayoutForward` / `LayoutRight` frame;
- centered rectangular slot layout;
- per-entity `DesiredSlotPosition`;
- `DesiredVelocity` diagnostics from the existing movement integration.

`EAlgonaMoveFacingMode` provides `FaceMovement` and `PreserveFacing` command
semantics. Existing input submits `FaceMovement`; `PreserveFacing` is an API
foundation only and has no player-facing control yet. Destination distance does
not select reverse or backpedal movement.

Console variables:

- `algona.P1.FormationEnabled`
- `algona.P1.FormationSlotSpacing`
- `algona.P1.FormationMaxColumns`
- `algona.P1.FormationCorridorProbeHalfWidth`
- `algona.P1.FormationCorridorSafetyMargin`
- `algona.P1.FormationCorridorSampleCount`
- `algona.P1.FormationAnchorLookAheadDistance`
- `algona.P1.FormationFunnelLookAheadDistance`
- `algona.P1.FormationFunnelLateralSpeedFraction`
- `algona.P1.FormationMinimumForwardSpeedScale`
- `algona.P1.FormationForwardSpeedInterpSpeed`
- `algona.P1.FormationOrientationInterpSpeed`
- `algona.P1.FormationMinSameGroupSpacing`
- `algona.P1.FormationLocalDeformationInterpSpeed`
- `algona.P1.FormationSettlingRadius`
- `algona.P1.FormationSettlingRequiredTicks`

The previous whole-group `EffectiveColumns -> Rows` choke behavior has been removed. `OriginalColumns`, `OriginalRows`, and `SlotIndex` now describe the stable original topology and do not change because a corridor narrows.

Current local-deformation behavior:

- the rectangular layout is an axis: for every desired travel direction, the implementation chooses the closer of that direction and its opposite as the physical layout representative;
- a 180-degree `FaceMovement` reversal therefore rotates travel/facing while preserving the physically equivalent rectangular slot lattice, without angle or distance thresholds;
- a 90-degree route turn still rotates the physical layout by approximately 90 degrees;
- while route-following, the formation anchor moves ahead of the group center by a bounded look-ahead distance instead of sitting at the distant waypoint/destination from the start;
- bounded group-level lateral `ANavigationData::Raycast` samples form a longitudinal corridor profile that extends ahead of and behind the original formation depth;
- raycast hit locations are interpreted as obstructed-edge hits only when the nav raycast reports blocked; unobstructed probes use the configured probe endpoint as available width;
- valid corridor samples are intersected at group level to shift the anchor laterally onto a formation-safe route spine without inflating the Recast agent radius;
- each member keeps stable original row/column identity but samples corridor width from its physical longitudinal position relative to the moving anchor;
- future samples that are no wider than the member's current cross-section are ranked by required lateral speed (`distance / time to boundary`); the most urgent sample begins convergence before the member reaches the narrowing;
- a wider sample ahead cannot release the current constraint, so re-expansion starts only after that member's own longitudinal position reaches wider space;
- the local target is always clamped to the member's current physical corridor bounds even when a future look-ahead sample controls the desired deformation;
- projected NavMesh sample centers contribute lateral bounds only; they no longer replace ideal row centers and therefore cannot shear an open-terrain rectangle diagonally;
- only that member's lateral spacing/offset is constrained, so front, inside, rear, and exited portions of one group can have different local widths;
- local lateral offsets interpolate toward the constrained target and restore the original slot progressively in wider space;
- each member reports the forward speed compatible with its required lateral displacement and distance to the controlling narrowing; Legacy movement conservatively aggregates these recommendations, while the MassStock spike keeps them diagnostic-only and relies on slot steering plus local avoidance;
- fixed-step movement limits only the route-forward velocity component while preserving lateral convergence speed, then applies bounded local separation;
- forward speed changes smoothly for an approaching boundary; only a member already outside its current hard corridor interval forces immediate zero route-forward speed while lateral correction continues;
- `Funneling`, `InsideChoke`, and `ReExpanding` are diagnostics only and never gate route progress; only final-destination `Settling` waits for member convergence;
- world/NavMesh width remains the hard bound; diagnostics mark `SoftOverlap` if a corridor is too narrow even for the configured minimum spacing.

Per-group movement/presentation controls:

```text
algona.P1.SetGroupMinSpacing <GroupId> <Spacing>
algona.P0.SetGroupDebugCubeSize <GroupId> <Size>
```

`MinSameGroupSpacing` is group-owned data for the current prototype and can differ between unit scales without Actor inheritance. Same-group separation only corrects distances below that minimum, so it does not expand deliberately compressed members back toward normal spacing. Debug cube size changes only the existing ISM instance scale and has no gameplay collision authority.

The A-D implementation does not yet claim extreme-choke correctness. If the original row cannot fit at `MinSameGroupSpacing`, temporary transit lanes/zipper ordering are the next phase; the current implementation exposes this condition through `SoftOverlapMembers` and does not reintroduce global topology remapping.

After the route follower reaches the final path point, the group enters `Settling` instead of immediately going idle. Formation slot targets continue to update until all sampled members are within `FormationSettlingRadius` of their final slots for `FormationSettlingRequiredTicks` consecutive fixed ticks.

Invalid or unprojectable destinations set navigation failure and stop/cancel the active command instead of allowing direct movement to a non-navigable target. While P1 navigation is enabled, Mass movement also requires a committed valid route before assigning velocity, so queued/requesting/failed routes cannot bypass the NavMesh through the legacy P0 direct target.

`algona.P1.DumpFormation` reports group phase, original topology, normal/minimum spacing, corridor profile summary, group forward-speed scale, speed-limited member count, local phase counts, soft-overlap count, debug cube size, and bounded member samples distributed across the stable slot range. Member samples include physical/controlling longitudinal offsets, distance to narrowing, required lateral distance/speed, and recommended forward scale. An explicit dump also draws corridor sample cross-sections and original-to-local target lines for those samples. No per-tick entity log is emitted.

The navigation and formation dumps also expose `MoveFacingMode`,
`TravelDirection`, `LayoutForward`, `LayoutRight`, `FacingDirection`, the layout
frame dot product, required footprint half-width, and safe anchor lateral
offset. Debug ISM instances use `FacingDirection` for yaw and a slightly
rectangular presentation-only scale so facing remains visible without
Actor-per-unit.

Rectangular geometry can be created without changing the benchmark spawn path:

```text
algona.P0.SpawnRect <GroupCount> <Columns> <Rows>
```

`algona.P0.Spawn` remains unchanged. Both commands reuse or create the single
debug ISM presentation actor through the existing development harness.

Extreme-choke transit lanes remain paused until the geometry, facing, about-face,
wall-corner footprint, and local-funnel runtime gate passes.

## Debug Mouse Input

`AP0SimulationPlayerController` now has debug-only RTS mouse input:

- Shift + LMB resolves the nearest Mass entity around the cursor world point and stores its `GroupId`.
- Shift + RMB submits the selected group through the existing `SubmitMoveGroupCommand` path.
- Plain LMB/RMB do not execute the P1 debug select/move path.
- In multiplayer, client-originated RMB still goes through the existing server RPC.

Console variables:

- `algona.P1.DebugMouseInput`
- `algona.P1.DebugPickRadius`

This is not production selection/input architecture.

## Deferred P1 Work

Not implemented in this vertical slice:

- production dynamic topology/Recast runtime geometry policy
- production hard-blocker authoring and replication
- P1 movement benchmark scenarios
- P1 multiplayer route smoke

## Local Steering Vertical Slice

P1.3 adds a minimal Mass-friendly local steering path:

- one uniform spatial grid is rebuilt once per fixed simulation step;
- grid entries are copied from Mass positions, group ids, entity handles, and entity radii;
- cells are keyed by integer XY cell coordinates;
- each moving entity queries only its current cell and the eight adjacent cells;
- neighbor accumulation is bounded by `algona.P1.NeighborMaxCandidates`;
- local separation uses the query radius only to find candidates; different-group correction uses entity radii plus optional `algona.P1.SeparationMinimumPadding`, while same-group correction uses the owning group's `MinSameGroupSpacing`;
- local separation is blended into the existing slot-target movement direction;
- no per-entity global pathfinding and no global pairwise neighbor search are used.

Console variables:

- `algona.P1.SpatialGridEnabled`
- `algona.P1.LocalSeparationEnabled`
- `algona.P1.SpatialGridCellSize`
- `algona.P1.NeighborQueryRadius`
- `algona.P1.NeighborMaxCandidates`
- `algona.P1.SeparationStrength`
- `algona.P1.SeparationMinimumPadding`
- `algona.P1.DesiredVelocityBlend`

`algona.P0.DumpState` and `algona.P1.DumpNavigation` report compact P1.3 counters:

- grid build ms;
- grid entity count;
- grid cell count;
- neighbor query ms;
- neighbor query count;
- total / average / max neighbor candidates;
- local avoidance ms.

`algona.P0.DumpState` and `algona.P1.DumpFormation` also report compact
selected-group rest stability diagnostics: max/average slot error, max/average
desired speed, max/average separation speed, moving-by-separation count, and
settled members.

This is a first local separation layer, not a final crowd solver, lane system, deadlock solver, or reservation model.

## Dynamic Obstacle / Repath Vertical Slice

P1.4 adds a debug Algona-side hard blocker layer for testing route
invalidation and recovery without registering every moving unit as Recast
geometry.

Implemented debug commands:

- `algona.P1.AddHardBlocker <X> <Y> <Z> <Radius>`
- `algona.P1.AddHardBlockerAtGroup <GroupId> [Radius=400]`
- `algona.P1.ClearHardBlockers`
- `algona.P1.DumpHardBlockers`

Console variables:

- `algona.P1.HardBlockersEnabled`
- `algona.P1.StuckDetectionEnabled`
- `algona.P1.StuckDistanceEpsilon`
- `algona.P1.StuckObservationTicks`
- `algona.P1.StuckRequiredTicks`
- `algona.P1.RepathCooldownTicks`

Behavior:

- adding a hard blocker invalidates active group routes whose compact path
  segments intersect the blocker;
- `algona.P1.AddHardBlockerAtGroup` places the blocker ahead of the current
  group center along the active route/formation direction, not at an arbitrary
  distant target, and guarantees a scheduler requeue for that selected active
  group;
- invalidation queues a budgeted repath through the existing P1 path scheduler;
- an existing compact route can remain active while the async repath is pending,
  so the movement clamp can stop entities against the debug blocker during the
  recovery interval;
- completed paths that still intersect an active hard blocker are rejected
  before becoming authoritative group routes;
- fixed-step Mass movement clamps proposed entity movement that would cross an
  active hard blocker, so entities do not pass through the debug blocker while
  waiting for route recovery;
- clearing blockers requeues active groups that were blocked by this debug
  layer;
- group-level stuck detection tracks center progress toward the destination and
  queues cooldown-limited repaths through the same scheduler.

`algona.P0.DumpState`, `algona.P1.DumpNavigation`, and
`algona.P1.DumpHardBlockers` report compact P1.4 counters:

- active hard blockers;
- routes invalidated by the latest blocker change;
- total repath requests;
- stuck groups;
- movement steps blocked by hard blockers;
- cumulative movement steps blocked by hard blockers;
- last repath reason and stuck ticks for the selected group.

Group-level stuck detection uses a fixed-tick observation window and accepts
progress from route point advancement, group-center displacement,
formation-anchor displacement, or destination-distance improvement. Short
formation turns, compression, and temporary local-separation slowdowns should
not regularly trigger stuck recovery on open terrain.

This is not production dynamic topology or a Recast runtime-geometry policy. It
is a P1.4 proof path for command-safe route invalidation, bounded repath, and
immediate Algona-side collision blocking.

Gate D runtime result:

- modifier-only debug mouse input worked, and plain LMB/RMB did not trigger the P1 debug select/move path;
- false-positive stuck recovery was no longer observed during normal movement;
- `AddHardBlockerAtGroup` placed the hard blocker ahead of the active group;
- active routes were invalidated and repath used the existing budgeted scheduler;
- routes through the hard blocker were rejected;
- movement clamp was observed through `TotalBlockedEntities > 0`;
- units visually did not pass through the hard blocker;
- `ClearHardBlockers` requeued the blocked group and produced a new route;
- no repath storm, Actor-per-unit path, per-entity global pathfinding, or per-entity log spam was observed.

## Navigation LOD / SimLOD Integration

P1.5 consumes the existing `FAlgonaSimLODFragment`; it does not introduce a
second classifier, subsystem, distance model, or navigation state machine.

Implemented cadence behavior:

- High entities retain the existing every-step movement, slot-target refresh,
  neighbor query, and local separation behavior.
- Medium and Low entities reuse their existing P0 `UpdateStride` and
  `TickOffset`; their last valid slot target and movement state remain usable
  between eligible updates.
- Each group caches a navigation tier derived conservatively from its members:
  the highest-detail member wins. Group formation geometry uses that tier's
  existing stride.
- Formation orientation and per-member local-deformation interpolation account
  for fixed-step/LOD cadence, so lower detail retains the same authoritative
  targets without introducing a second navigation tier system.
- Route submission/completion, route progress, hard-blocker invalidation,
  repath, movement clamping, and stuck recovery remain correctness paths and
  are not delayed by Navigation LOD.
- Settling forces formation target refresh so arrival convergence semantics are
  retained.
- The existing single spatial grid is still rebuilt with all entities. Lower
  tiers reduce expensive neighbor/separation queries rather than creating
  tier-specific grids.

Development diagnostic command:

```text
algona.P1.SetSimLOD <High|Medium|Low> [GroupId]
```

Without `GroupId`, the command updates the existing P0 SimLOD fragments for the
whole authoritative population. It also refreshes the derived group tiers and
resets the P1.5 measurement window. This command is a debug harness for Gate E,
not a production importance classifier.

`algona.P0.DumpState` now reports:

- entity and derived group counts by High/Medium/Low;
- formation groups and formation targets updated/skipped by LOD;
- neighbor queries executed/skipped and executed counts by tier;
- avoidance applications;
- latest Formation/Grid/Neighbor/Avoidance/Movement timings;
- measurement-window average timings and cumulative executed/skipped workload.

The P1.5 implementation is statically validated. Runtime stability and the
expected workload/CPU reduction remain Gate E evidence, not a completed
benchmark claim.

## Alternative Stock Mass Local-Motion Spike

This is an isolated technical spike, not a migration decision. `Legacy`
remains the default local-motion mode and retains its existing archetype,
spatial grid, separation, and integration path.

Development mode selection:

```text
algona.P1.LocalMotionMode Legacy|MassStock
```

The selected mode applies to the next `algona.P0.Spawn` or
`algona.P0.SpawnRect`. An existing population is not migrated in place, which
keeps each entity on exactly one local-motion archetype and avoids mixed
integrators during comparison.

`MassStock` preserves the existing command, group route, corridor, formation
anchor, stable slot, destination, hard-blocker, and fixed-step ownership. Its
lower path is:

```text
Algona desired member target
-> FMassMoveTargetFragment
-> UMassSteerToMoveTargetProcessor
-> UMassMovingAvoidanceProcessor
-> UMassApplyForceProcessor
-> Algona authoritative fixed-step integration adapter
```

The stock processors are executed manually on the MassStock archetype
collection with the P0 fixed delta. `FMassCustomMovementTag` prevents the
globally registered `UMassApplyMovementProcessor` from changing entity
transforms at render-frame cadence. Before every authoritative step, transient
stock desired-movement, force, and steering inputs are restored from the last
authoritative stock velocity, so render-frame transient processing cannot
change the fixed-step result.

The adapter also drives the stock `FMassMoveTargetFragment` action lifecycle.
An active, navigation-ready group uses `Move`; an inactive or pending group
uses `Stand`. This keeps moving avoidance out of the completed-command rest
state instead of allowing neighboring settled members to repel each other
indefinitely.

MassStock no longer applies the minimum member
`RecommendedForwardSpeedScale` to the entire group. That aggregation made one
late lateral convergence request reduce all members to the 0.15 floor and was
a direct source of the observed bridge slowdown. Member recommendations remain
available as formation diagnostics, while MassStock submits the configured
gameplay movement speed to the stock move target. Steering toward each local
slot still contains the required lateral convergence; no gameplay speed was
increased.

The final integration adapter copies stock desired velocity into the Algona
authoritative position/velocity, applies the existing hard-blocker constraint,
then resolves the predicted fixed-step displacement against the group corridor
as a swept local kinematic constraint. At the earliest boundary contact it
removes only outward-normal motion and preserves the tangential remainder, so
members slide instead of being projected several spacings inward. An entity
that already starts outside the sampled clearance receives only a bounded
recovery of at most 10 percent of its radius per step. The final positional
correction is a small numerical safety net and is measured separately from
normal motion clipped at contact.

The swept constraint reuses the group corridor profile and reserves
`SafetyMargin + AgentRadius` from each sampled edge; it does not issue
per-entity NavMesh queries. Desired local targets use the same clearance in
MassStock mode. Stock navigation edges remain predictive soft steering input,
while the Algona constraint is the authoritative no-penetration layer. This
makes the current experiment a hybrid path rather than a claim that stock
movement integration is fully adopted.

Stock moving avoidance uses the engine `UMassNavigationSubsystem` obstacle
hash grid. `MinSameGroupSpacing * 0.5` is mapped to the experimental agent
radius/circle collider, with a 10 cm lower bound. UE 5.8.1 defaults still apply
to `SeparationRadiusScale`, `PredictiveAvoidanceRadiusScale`,
`StaticObstacleClearanceScale`, and the avoidance distances; no avoidance
parameter was changed before runtime force diagnostics identify the dominant
layer. This mapping is spike-only and is not a gameplay invariant.
`MassNavigation` is supplied by UE 5.8.1's
experimental `MassAI` plugin, which is already enabled for this project; that
experimental dependency is part of the eventual migration risk assessment.

Static wall input is generated without ZoneGraph or per-entity global paths.
Each member receives at most 16 nearby inward-facing segments derived from the
existing group corridor samples. Distant corridor edges are omitted. Algona's
group Recast route and hard-blocker constraint remain authoritative.

MassStock currently runs formation targets and stock local motion at full
cadence regardless of the existing entity SimLOD tier. No second navigation
LOD was introduced. Medium/Low cadence compatibility remains a follow-up only
if the correctness and performance spike is accepted.

Known experiment limitation: obstacle-grid, smooth-height, steering, moving
avoidance, and apply-force processors are auto-registered in the engine Mass
frame pipeline for this matching archetype. They can perform duplicate or
non-authoritative transient work even though custom movement blocks frame
transform integration and the fixed-step mapper restores authoritative inputs.
This preserves authoritative fixed-step semantics but adds real CPU cost; the
10k comparison must include that cost when deciding GO/PARTIAL/NO-GO.

Compact diagnostics:

```text
algona.P1.DumpLocalMotion [GroupId]
algona.P1.RunLocalMotionSpikeSuite [GroupSize=100] [FixedStepsPerCase=64]
```

The dump reports desired/actual/forward speed, slot error, agent radius and
spacing, local corridor width, same-group neighbors within the stock separation
range, swept-contact count, bounded safety correction, residual penetration,
route/bridge/settling timing, and Algona-visible processor timings. While an
active MassStock group is moving, one dump also arms a single next-fixed-step
capture over up to eight stable-distribution samples. It logs steering force,
agent-only avoidance force from a restored no-edge diagnostic pass, and the
remaining environment-force contribution. The diagnostic duplicate pass is
timed and excluded from the reported normal avoidance duration; it is never a
continuous per-entity log path.

The suite automatically
compares Legacy and MassStock at 100, 1k, and 10k in navigation-disabled,
SimLOD-disabled simulation cases. Those suite timings cover the authoritative
fixed-step scopes only. The extra global-frame transient processor cost must
be assessed separately in the running PIE comparison. No runtime or
performance result is claimed until that gate is executed.
