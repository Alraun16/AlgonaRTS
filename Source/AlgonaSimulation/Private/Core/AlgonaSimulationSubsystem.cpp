#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "MassSpawnerSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
	// Read when a world starts. Restart PIE after changing these values.
	TAutoConsoleVariable<int32> CVarAlgonaP0UnitCount(
		TEXT("algona.P0.UnitCount"),
		AlgonaSimulationDefaults::UnitCount,
		TEXT("Number of authoritative Mass units created at world begin play."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP0SquadSize(
		TEXT("algona.P0.SquadSize"),
		AlgonaSimulationDefaults::SquadSize,
		TEXT("Requested number of units in one P0/P1 squad."),
		ECVF_Default);
}

bool UAlgonaSimulationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}

	const bool bPlayableWorld =
		World->WorldType == EWorldType::Game
		|| World->WorldType == EWorldType::PIE;

	// Simulation runs in standalone and on the authoritative server, not on
	// ordinary network clients.
	return bPlayableWorld && World->GetNetMode() != NM_Client;
}

void UAlgonaSimulationSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	MassEntitySubsystem =
		Collection.InitializeDependency<UMassEntitySubsystem>();
	MassSpawnerSubsystem =
		Collection.InitializeDependency<UMassSpawnerSubsystem>();

	if (MassEntitySubsystem)
	{
		InitializeQueries();
	}
}

void UAlgonaSimulationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	(void)InWorld;

	FixedStepAccumulator.Reset();
	SimulationTick = 0;
	StateRevision = 0;
	Metrics = FAlgonaSimulationMetrics();

	if (!IsAuthoritativeSimulationWorld())
	{
		return;
	}

	if (!MassEntitySubsystem || !MassSpawnerSubsystem)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::MissingMassServices;
		return;
	}

	const int32 UnitCount = FMath::Clamp(
		CVarAlgonaP0UnitCount.GetValueOnGameThread(),
		1,
		500000);

	const int32 SquadSize = FMath::Clamp(
		CVarAlgonaP0SquadSize.GetValueOnGameThread(),
		1,
		1000);

	if (!CreateUnits(UnitCount, SquadSize))
	{
		DestroyUnits();
		return;
	}

	// First complete authoritative state is now available to Presentation.
	++StateRevision;

	Metrics.StartupState = EAlgonaSimulationStartupState::Ready;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	UnitUpdateQuery.Reset();
	UnitSnapshotQuery.Reset();

	// During world teardown Mass can already be deinitialized, so do not call
	// DestroyUnits() here. The world owns and tears down the entity manager.
	UnitEntities.Reset();
	Squads.Reset();
	SquadEntityRanges.Reset();
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	UnitSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	PendingMoveCommands.Reset();
	UnitEntityConfig = nullptr;

	Metrics.EntityCount = 0;
	Metrics.SquadCount = 0;

	MassSpawnerSubsystem = nullptr;
	MassEntitySubsystem = nullptr;
	FixedStepAccumulator.Reset();

	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	if (Metrics.StartupState != EAlgonaSimulationStartupState::Ready
		|| !IsAuthoritativeSimulationWorld()
		|| !MassEntitySubsystem
		|| UnitEntities.IsEmpty())
	{
		return;
	}

	Metrics.LastExecutedStepsThisFrame =
		FixedStepAccumulator.Advance(
			static_cast<double>(DeltaTime),
			[this](double StepSeconds)
			{
				RunSimulationStep(static_cast<float>(StepSeconds));
			});

	Metrics.BacklogSeconds = FixedStepAccumulator.GetBacklogSeconds();
	Metrics.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();
}

TStatId UAlgonaSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UAlgonaSimulationSubsystem,
		STATGROUP_Tickables);
}

FAlgonaSimulationMetrics UAlgonaSimulationSubsystem::GetSimulationMetrics() const
{
	FAlgonaSimulationMetrics Snapshot = Metrics;
	Snapshot.SimulationTick = SimulationTick;
	Snapshot.EntityCount = UnitEntities.Num();
	Snapshot.SquadCount = Squads.Num();
	Snapshot.BacklogSeconds = FixedStepAccumulator.GetBacklogSeconds();
	Snapshot.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();
	return Snapshot;
}

int32 UAlgonaSimulationSubsystem::ExportUnitSnapshots(
	TArray<FAlgonaUnitSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSnapshots);

	OutSnapshots.Reset();

	if (!MassEntitySubsystem
		|| !UnitSnapshotQuery
		|| MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SafeMaxEntities = FMath::Min(
		MaxEntities,
		UnitEntities.Num());

	if (SafeMaxEntities <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SafeMaxEntities);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	UnitSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[&OutSnapshots, SafeMaxEntities](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();
			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities()
					&& OutSnapshots.Num() < SafeMaxEntities;
				++Index)
			{
				const FTransform& Transform =
					Transforms[Index].GetTransform();

				FAlgonaUnitSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();
				Snapshot.EntityId = Ids[Index].Value;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		});

	return OutSnapshots.Num();
}

bool UAlgonaSimulationSubsystem::SubmitMoveSquadCommand(
	int32 SquadId,
	const FVector& TargetLocation)
{
	if (!IsAuthoritativeSimulationWorld()
		|| !Squads.IsValidIndex(SquadId)
		|| Squads[SquadId].SquadId != SquadId)
	{
		return false;
	}

	FAlgonaSquadMoveCommand& Command =
		PendingMoveCommands.AddDefaulted_GetRef();
	Command.SquadId = SquadId;
	Command.TargetLocation = TargetLocation;
	return true;
}

int32 UAlgonaSimulationSubsystem::SubmitMoveAllSquadsByOffset(
	const FVector& Offset)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		return 0;
	}

	int32 SubmittedCommands = 0;

	for (const FAlgonaSquad& Squad : Squads)
	{
		if (SubmitMoveSquadCommand(
			Squad.SquadId,
			Squad.AnchorLocation + Offset))
		{
			++SubmittedCommands;
		}
	}

	return SubmittedCommands;
}

void UAlgonaSimulationSubsystem::InitializeQueries()
{
	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	UnitUpdateQuery =
		MakeUnique<FMassEntityQuery>(EntityManager.AsShared());
	UnitUpdateQuery->AddRequirement<FTransformFragment>(
		EMassFragmentAccess::ReadWrite);
	UnitUpdateQuery->AddRequirement<FAlgonaUnitIdFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitUpdateQuery->AddRequirement<FAlgonaSquadMemberFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitUpdateQuery->AddRequirement<FAlgonaUnitMovementFragment>(
		EMassFragmentAccess::ReadWrite);
	UnitUpdateQuery->AddTagRequirement<FAlgonaUnitTag>(
		EMassFragmentPresence::All);

	UnitSnapshotQuery =
		MakeUnique<FMassEntityQuery>(EntityManager.AsShared());
	UnitSnapshotQuery->AddRequirement<FAlgonaUnitIdFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitSnapshotQuery->AddRequirement<FTransformFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitSnapshotQuery->AddRequirement<FAlgonaSquadMemberFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitSnapshotQuery->AddTagRequirement<FAlgonaUnitTag>(
		EMassFragmentPresence::All);
}

bool UAlgonaSimulationSubsystem::IsAuthoritativeSimulationWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}
