# AGENTS.md — Chronicles of Algona / AlgonaRTS

Repository-level guidance for Codex and other coding agents working on **Chronicles of Algona / AlgonaRTS**.

The goal of this file is to preserve durable architectural and workflow rules. Phase-specific task files and the user's current request may add stricter requirements.

---

## Project and current stage

```text
Engine: Unreal Engine 5.8.x
Project: AlgonaRTS
Root game module: AlgonaRTS
Mass simulation module: AlgonaSimulation
```

**P0 — Mass Simulation Technical Foundation is complete.** Preserve that foundation.

The current development focus is **P1 — Movement / Pathfinding**, but the user's current task and the current repository state take precedence over this status line if the project has moved on.

Do not treat old P0 execution documents as the active plan unless the user explicitly asks to work on P0.

---

## Source of truth

Before implementing, use this priority order:

1. the user's current request and explicit constraints;
2. the current repository code and configuration;
3. the current active task/plan/documentation in the repository;
4. locally installed Unreal Engine 5.8 source and headers;
5. official Epic documentation when external documentation is needed;
6. Graphify as a navigation/indexing aid, never as the final source of truth.

Rules:

- Prefer current code over assumptions, stale plans, and old task text.
- Verify version-specific Unreal/Mass APIs against the locally installed UE 5.8 source/headers where practical.
- Do not invent engine APIs.
- Do not recreate classes, modules, files, or systems that already exist and are valid.
- If an old task specification conflicts with newer working code, do not blindly overwrite the newer implementation. Identify the conflict and preserve the newer valid state unless the current task explicitly requires a migration.
- Documentation must describe implemented reality, not aspirational design.

---

## Core architectural invariants

Do not change these without a measured, demonstrated, or explicitly approved architectural reason.

### Simulation foundation

`AlgonaSimulation` owns RTS simulation orchestration.

Unreal Mass Entity is the data-oriented entity storage and bulk-processing foundation.

Expected dependency direction:

```text
AlgonaRTS
    ↓
AlgonaSimulation
    ↓
MassEntity
```

Avoid circular dependencies.

### World lifetime

Authoritative runtime simulation state is world-scoped.

Use `UWorld`-scoped subsystem lifetime for simulation services.

Do not store authoritative world state in:

```text
static global objects
module singletons
process-wide managers
```

### Mass entities

Mass combat entities must not require:

```text
Actor + Character + AIController + per-entity Tick
```

Do not introduce Actor-per-unit architecture as a convenience shortcut.

### Simulation / presentation separation

Authoritative simulation state is separate from presentation.

Allowed direction:

```text
Simulation -> Presentation
```

Presentation must not become the source of authoritative entity positions or gameplay state.

Prototype ISM/debug presentation is acceptable where explicitly used for testing, but it must remain presentation-only.

### Multiplayer

The architecture must remain server-authoritative-capable.

Clients may submit high-level commands.

Clients must not directly mutate authoritative simulation state.

Do not create replicated Actor-per-Mass-entity architecture.

### Scale

All hot-path decisions must remain credible for:

```text
10 000+ active combat entities
```

Use representative stress populations such as:

```text
1k
5k
10k
20k
```

when scale testing is relevant. Larger ceiling tests are diagnostic, not proof of production capacity.

---

## Movement / pathfinding contract

The accepted mass-movement pipeline is:

```text
Move Command
    ↓
Group Route
    ↓
Navigation Corridor
    ↓
Formation Anchor / Frame
    ↓
Entity Slot Targets
    ↓
Local Steering / Avoidance
    ↓
Fixed-step Integration
```

Rules:

- Global pathfinding is group-level, not per entity.
- Individual Mass entities must not run full global NavMesh paths.
- Unreal Recast/NavMesh owns authoritative static traversability and group-level global path queries.
- `AlgonaSimulation` owns group navigation state, request scheduling, route lifecycle, formation state, corridor-aware formation behavior, local spatial data, steering/avoidance, stuck/recovery logic, and navigation SimLOD.
- Formation targets must remain compatible with the walkable corridor; P1 local avoidance must not be used to hide fundamentally invalid formation geometry.
- Stable member-to-slot identity should be preserved as far as practical during turns, compression, and re-expansion.
- Route arrival and formation settling are distinct concerns; reaching the group anchor is not automatically equivalent to all members being settled.
- Do not switch to Detour Crowd, UE RVO, `AIController`/unit, `CharacterMovementComponent`/unit, `PathFollowing`/unit, custom global navmesh, HPA*, or flow fields without measured need and explicit architectural review.

---

## Unreal / Mass dependency policy

`MassEntity` is an engine runtime module; do not search for or require a separate "Mass Entity" plugin toggle.

Do not enable extra Mass Gameplay modules/plugins merely because they exist.

Before adding dependencies such as:

```text
MassGameplay
MassRepresentation
MassSpawner
MassCrowd
MassNavigation
MassStateTree
MassReplication
```

there must be a concrete need for the active task.

Remove dependencies that are no longer used.

Keep public dependencies minimal. If a Mass type does not appear in a module's public API, prefer a private dependency where practical.

---

## Implementation style

Prefer:

- data-oriented structures;
- compact fragments;
- bulk operations;
- stable archetypes;
- explicit ownership;
- explicit simulation stages;
- fixed-step behavior where required by the simulation contract;
- measurable code;
- simple algorithms that test architecture cleanly before adding complexity.

Avoid in hot entity loops:

```text
per-entity heap allocations
per-entity UObject lookups
per-entity locks
per-entity world traces
per-entity timers
per-entity Actors
per-entity global navigation queries
```

Do not optimize blindly. Instrument first when practical.

Before editing a file, inspect the existing implementation.

Do not perform unrelated large refactors.

Do not leave dead prototype code after a replacement is validated.

---

# Graphify workflow

This project has a code knowledge graph under `graphify-out/` and a project-scoped Graphify/Codex integration.

Graphify is intended to reduce broad repository search and repeated raw-file reading. It does **not** replace source inspection.

## Query-first rule

For non-trivial codebase questions, architecture tracing, dependency discovery, or unfamiliar code areas, use Graphify first when `graphify-out/graph.json` exists.

Typical commands:

```text
graphify query "<question>"
graphify explain "<concept>"
graphify path "<A>" "<B>"
graphify affected "<concept>"
```

Use the scoped result to identify the relevant files/classes/functions, then read the real source before editing.

If the user points to an exact file/line and the task is purely local, do not run Graphify only as a ritual.

When the user explicitly types `$graphify`, use the installed Graphify skill/instructions first.

## Raw source remains authoritative

Treat the real:

```text
.h
.cpp
.Build.cs
project/config files
```

as the final source of truth.

Before architectural, cross-system, destructive, or behavior-changing edits, verify the relevant Graphify result against raw source.

If Graphify and raw source disagree, raw source wins.

## Unreal-specific limitation

Graphify can partially miss or mis-resolve Unreal C++ relationships involving:

```text
UCLASS
USTRUCT
UFUNCTION
UPROPERTY
GENERATED_BODY
reflection-generated code
qualified/static calls
```

Therefore:

- `graphify affected` is not exhaustive impact analysis for this repository;
- `graphify path` is not guaranteed to contain every real call/dependency path;
- verify important impact/caller conclusions with scoped raw-source search and direct file inspection.

## Graphify update policy

**Do not run `graphify update Source` after every small edit.**

Use this policy:

1. **At the start of a task/session:** if project source has changed since the graph was last built and Graphify will be used, update the graph once.
2. Use Graphify to localize the relevant area, then inspect raw source.
3. Make a cohesive batch of related edits without rebuilding the graph between every file change.
4. After a **meaningful code batch**, update the graph once **before the next cross-system / dependency / impact analysis that relies on Graphify**.
5. Before a final architecture or impact review, update the graph once if the graph may be stale.
6. For a small isolated edit where no further Graphify query depends on the new state, the update may wait until the next task/session.

The intended pattern is:

```text
update when needed
→ query
→ inspect raw source
→ implement a coherent batch
→ update before relying on the graph again
```

not:

```text
edit one line
→ update
→ edit one line
→ update
```

Code-only AST updates are cheap, but unnecessary update spam still wastes time and tool calls.

## Graph artifacts and hooks

Current repository policy:

- do not install Graphify git auto-update hooks unless the user explicitly approves it;
- do not add/commit `graphify-out/` merely because Graphify recommends it;
- do not rewrite `.gitignore` for Graphify without an explicit reason;
- project integration files such as `AGENTS.md` and `.codex/` must be merged carefully, not overwritten blindly.

Read `GRAPH_REPORT.md` for broad architecture review only when query/path/explain are insufficient.

---

# Verification and Codex budget policy

The user has a limited Codex budget.

**Do not build, launch Unreal Editor, run PIE, Standalone, packaged builds, benchmarks, or long runtime tests after every small phase.**

Default workflow:

```text
inspect
    ↓
Graphify/local source/API investigation as appropriate
    ↓
implement a coherent batch
    ↓
static/API validation
    ↓
continue through related work
    ↓
request one verification gate only when runtime evidence is genuinely needed
```

Use builds and runtime tests as **verification gates**, not as routine steps after every edit.

## Static / low-cost validation

Perform freely while implementing:

```text
repository inspection
Graphify scoped queries
configuration inspection
local UE 5.8 header/source inspection
API/signature validation
dependency validation
reflection macro validation
code-path reasoning
ownership/lifetime tracing
documentation consistency checks
```

Do not request a build merely to check something determinable from local source/API.

## Expensive verification gates

Do not run these automatically unless the current user instruction explicitly allows it:

```text
full build
Unreal Editor launch
PIE
Standalone
packaged build
Unreal Insights capture
large entity benchmark
multiplayer runtime test
```

A verification gate is justified when continuing without runtime evidence would create material risk, for example:

- substantial UE reflection/module/subsystem integration;
- compile correctness remains genuinely uncertain after local API inspection;
- new Mass fragments/processors/archetypes need runtime confirmation;
- runtime lifecycle/formation/navigation behavior is the question;
- performance benchmark work;
- multiplayer smoke/scale validation;
- final Definition of Done.

Combine related changes into one shared verification request.

Do not stop after every subtask if related work can continue safely.

---

# User verification requests

When a build or manual Unreal check is required, ask the user **in Russian**.

Use this structure:

```text
НУЖНА ПРОВЕРКА

Что изменилось с прошлой проверки:
<кратко: какие изменения/новый runtime path сейчас проверяются>

Что проверить:
<build / PIE / standalone / multiplayer / benchmark>

Что запустить:
<только реально необходимые команды или действия>

Ожидаемый результат:
<конкретные строки лога, поведение, значения или отсутствие ошибок>

Если не получилось:
<какой первый релевантный compiler error / лог / screenshot прислать>

Зачем это проверяем сейчас:
<что именно нельзя безопасно подтвердить статически>
```

Keep the request concise and actionable.

### Do not spam verification commands

- Stable development/debug values that are already defaults in code/config must **not** be repeated in every test request.
- Show only overrides required for that specific test.
- If many temporary overrides are repeatedly needed, prefer one debug preset/profile command instead of a long CVar list.
- Entity population commands such as `algona.P0.Spawn <count> <group size>` may remain manual when population is test-specific.
- Request `Dump...` commands only when their output is actually needed to make the next decision.
- Do not make the user re-enter settings merely because Codex can list them.

After the user returns results:

- continue from the current phase;
- do not restart the task;
- do not repeat completed work;
- use the returned evidence to fix the relevant problem or proceed.

---

## Benchmark discipline

When performance is being measured, record the relevant context:

```text
UE version
build configuration
hardware
entity count
simulation Hz
test configuration
average timing
p95
p99
memory observations
presentation mode
```

Do not present FPS alone as proof of simulation scalability.

Distinguish:

```text
simulation bottleneck
presentation bottleneck
navigation bottleneck
network bottleneck
```

Do not claim a bottleneck without measurement.

Prefer clean Development Standalone for formal performance conclusions when practical; PIE results are useful smoke/baseline evidence but must be labeled as such.

Keep a simulation-only benchmark path so presentation/rendering cost can be separated from simulation cost.

---

## Multiplayer rule

Multiplayer is a project invariant, even when a phase is not implementing production replication.

Preserve:

```text
server authority
high-level command submission
no replicated Actor per simulation entity
```

Prototype/debug synchronization is acceptable only when clearly documented as temporary.

Do not implement production replication/prediction/rollback merely because a smoke test exists unless it is part of the active task.

---

## Build failures and blockers

Do not guess through Unreal API/compiler errors.

If a build/runtime failure is provided:

1. read the first relevant compiler/linker/runtime error;
2. inspect the referenced project code;
3. inspect local UE 5.8 headers/source when the issue is engine/API-related;
4. identify whether the cause is API version, module dependency, lifecycle, reflection, navigation, Mass usage, or implementation;
5. make the smallest correct fix;
6. request another build only if runtime/compile evidence is actually needed.

Do not enter repeated build/fix/build loops unless explicitly allowed.

If the required fix would change a fundamental architecture decision, stop that branch and report:

```text
reproduction
error/log
affected code
measurements/evidence
options
recommended option
```

Do not silently redesign the project.

---

## Git

Do not force-push or rewrite history.

Do not push unless the user explicitly requests it.

If `.git` is unavailable:

- do not block technical work;
- skip commit-related actions;
- report the limitation when relevant.

If commits are available and part of the active task:

- one logical completed block per commit;
- do not commit known-broken code;
- do not mix unrelated files;
- use descriptive commit messages.

A build is not required before every commit under the budget policy. If a block has not passed its runtime verification gate, make that status explicit.

---

## Documentation

Documentation must describe the current implemented state.

When implementation changes an architectural assumption, update the corresponding documentation in the same logical work block when practical.

Clearly distinguish:

```text
implemented
statically validated
runtime verified
benchmark verified
prototype-only
not yet verified
```

Do not claim runtime verification if only static inspection was performed.

Do not preserve obsolete phase documentation as if it were current merely because it existed previously.

---

## Escalation / stop conditions

Stop autonomous implementation and report before changing fundamental architecture if evidence suggests that the current approach requires one of the following:

```text
abandoning group-level global pathfinding
introducing per-entity global navigation
replacing the fixed-step/group/SimLOD foundations
changing the server-authority command boundary
introducing a custom global navigation architecture
changing production multiplayer replication architecture
major redesign caused by a version-specific UE 5.8 engine limitation
```

A build being useful is **not** a stop condition; request a verification gate instead.

---

## Completion rule

Do not claim a phase/task complete until its actual Definition of Done or current user acceptance criteria are satisfied.

It is valid to report:

```text
implementation complete
runtime verification pending
```

when that is the true state.

At completion, report only relevant items, such as:

```text
implemented systems/changes
static validation performed
runtime tests actually performed
benchmark/multiplayer results when applicable
known limitations
prototype-only decisions
deferred work
changed files
commit SHA if available
```

Never claim that an unexecuted test passed.
