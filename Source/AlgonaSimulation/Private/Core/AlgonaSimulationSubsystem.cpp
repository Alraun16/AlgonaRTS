#include "Core/AlgonaSimulationSubsystem.h"

// Authoritative soldier fragments queried by the fixed-step core.
#include "Army/AlgonaSoldierFragments.h"

// Unreal world/Mass services and profiling used by the subsystem.
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
	TAutoConsoleVariable<int32> CVarAlgonaP0SoldierCount(
		TEXT("algona.P0.SoldierCount"),
		AlgonaSimulationDefaults::SoldierCount,
		TEXT("Number of authoritative Mass soldiers created at world begin play."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP0SquadSize(
		TEXT("algona.P0.SquadSize"),
		AlgonaSimulationDefaults::SquadSize,
		TEXT("Requested number of soldiers in one Algona squad."),
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

	// Simulation exists only where authoritative gameplay state is allowed.
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

	// Reset all world-local counters before creating the authoritative army.
	FixedStepAccumulator.Reset();
	SimulationTick = 0;
	StateRevision = 0;
	Metrics = FAlgonaSimulationMetrics();
	RecoveredLostSoldierIndices.Reset();

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

	const int32 SoldierCount = FMath::Clamp(
		CVarAlgonaP0SoldierCount.GetValueOnGameThread(),
		1,
		500000);

	const int32 SquadSize = FMath::Clamp(
		CVarAlgonaP0SquadSize.GetValueOnGameThread(),
		1,
		1000);

	if (!CreateSoldiers(SoldierCount, SquadSize))
	{
		DestroySoldiers();
		return;
	}

	// First complete authoritative state is now available to Presentation.
	++StateRevision;

	Metrics.StartupState = EAlgonaSimulationStartupState::Ready;
	Metrics.EntityCount = SoldierEntities.Num();
	Metrics.SquadCount = Squads.Num();
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	SoldierUpdateQuery.Reset();
	SoldierSnapshotQuery.Reset();

	// During world teardown Mass can already be deinitialized, so do not call
	// DestroySoldiers() here. The world owns and tears down the entity manager.
	SoldierEntities.Reset();
	Squads.Reset();
	SquadEntityRanges.Reset();
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	PendingMoveCommands.Reset();
	RecoveredLostSoldierIndices.Reset();
	SoldierEntityConfig = nullptr;

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
		|| SoldierEntities.IsEmpty())
	{
		return;
	}

	// Render frame time only feeds the accumulator; authoritative work executes
	// in fixed 40 Hz steps and can catch up after a slow frame without time loss.
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
	Snapshot.EntityCount = SoldierEntities.Num();
	Snapshot.SquadCount = Squads.Num();
	Snapshot.BacklogSeconds = FixedStepAccumulator.GetBacklogSeconds();
	Snapshot.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();
	return Snapshot;
}

int32 UAlgonaSimulationSubsystem::ExportSoldierSnapshots(
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSnapshots);

	OutSnapshots.Reset();

	if (!MassEntitySubsystem
		|| !SoldierSnapshotQuery
		|| MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SafeMaxEntities = FMath::Min(
		MaxEntities,
		SoldierEntities.Num());

	if (SafeMaxEntities <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SafeMaxEntities);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	// P1 full-export fallback remains renderer-neutral and chunk-contiguous.
	SoldierSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[&OutSnapshots, SafeMaxEntities](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaSoldierIdFragment> Ids =
				Context.GetFragmentView<FAlgonaSoldierIdFragment>();
			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities()
					&& OutSnapshots.Num() < SafeMaxEntities;
				++Index)
			{
				const FTransform& Transform =
					Transforms[Index].GetTransform();

				FAlgonaSoldierSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();
				Snapshot.EntityId = Ids[Index].Value;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		});

	return OutSnapshots.Num();
}

bool UAlgonaSimulationSubsystem::GetSquadCenter(
	int32 SquadId,
	FVector& OutCenter) const
{
	if (!Squads.IsValidIndex(SquadId)
		|| Squads[SquadId].SquadId != SquadId)
	{
		return false;
	}

	OutCenter = Squads[SquadId].GetSpatialCenter();
	return true;
}

bool UAlgonaSimulationSubsystem::GetSquadCommandPreview(
	int32 SquadId,
	FAlgonaSquadCommandPreview& OutPreview) const
{
	if (!Squads.IsValidIndex(SquadId)
		|| Squads[SquadId].SquadId != SquadId)
	{
		return false;
	}

	const FAlgonaSquad& Squad = Squads[SquadId];
	OutPreview.Center = Squad.AnchorLocation;
	OutPreview.ActiveUnitCount = Squad.ActiveMemberSoldierIndices.Num();
	OutPreview.CurrentMaxSlotsPerRow = Squad.Formation.MaxSlotsPerRow;
	OutPreview.SlotSpacingCm = Squad.SoldierSpacingCm;
	return true;
}

bool UAlgonaSimulationSubsystem::SubmitMoveSquadCommand(
	int32 SquadId,
	const FVector& TargetLocation,
	EAlgonaSquadMovePace Pace,
	const FVector& FinalFacingDirection,
	int32 RequestedMaxSlotsPerRow)
{
	if (!IsAuthoritativeSimulationWorld()
		|| !Squads.IsValidIndex(SquadId)
		|| Squads[SquadId].SquadId != SquadId)
	{
		return false;
	}

	// Commands are queued and become authoritative only at the next fixed step.
	FAlgonaSquadMoveCommand& Command =
		PendingMoveCommands.AddDefaulted_GetRef();
	Command.SquadId = SquadId;
	Command.TargetLocation = TargetLocation;
	Command.Pace = Pace;
	Command.FinalFacingDirection = FinalFacingDirection.GetSafeNormal2D();
	Command.RequestedMaxSlotsPerRow = FMath::Max(0, RequestedMaxSlotsPerRow);
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
			Squad.AnchorLocation + Offset,
			EAlgonaSquadMovePace::Run))
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

	// One authoritative hot-loop query visits each soldier exactly once per
	// fixed step and also accumulates cohesion distance categories.
	SoldierUpdateQuery =
		MakeUnique<FMassEntityQuery>(EntityManager.AsShared());
	SoldierUpdateQuery->AddRequirement<FAlgonaSoldierIdFragment>(
		EMassFragmentAccess::ReadOnly);
	SoldierUpdateQuery->AddRequirement<FTransformFragment>(
		EMassFragmentAccess::ReadWrite);
	SoldierUpdateQuery->AddRequirement<FAlgonaSquadMemberFragment>(
		EMassFragmentAccess::ReadOnly);
	SoldierUpdateQuery->AddRequirement<FAlgonaSoldierMovementFragment>(
		EMassFragmentAccess::ReadWrite);
	SoldierUpdateQuery->AddTagRequirement<FAlgonaSoldierTag>(
		EMassFragmentPresence::All);

	// P1 snapshot query is intentionally unchanged in shape.
	SoldierSnapshotQuery =
		MakeUnique<FMassEntityQuery>(EntityManager.AsShared());
	SoldierSnapshotQuery->AddRequirement<FAlgonaSoldierIdFragment>(
		EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery->AddRequirement<FTransformFragment>(
		EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery->AddRequirement<FAlgonaSquadMemberFragment>(
		EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery->AddTagRequirement<FAlgonaSoldierTag>(
		EMassFragmentPresence::All);
}

bool UAlgonaSimulationSubsystem::IsAuthoritativeSimulationWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}
