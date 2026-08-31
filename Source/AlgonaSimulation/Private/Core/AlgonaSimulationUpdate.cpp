#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaSoldierFragments.h"

#include "HAL/PlatformTime.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_LOG_CATEGORY_STATIC(
	LogAlgonaSpatialGridBenchmark,
	Log,
	All);

namespace
{
	constexpr uint64 SpatialBenchmarkWindowSteps = 200;

	const TCHAR* GetSpatialGridModeName(
		EAlgonaSpatialGridMode Mode)
	{
		switch (Mode)
		{
		case EAlgonaSpatialGridMode::None:
			return TEXT("None");

		case EAlgonaSpatialGridMode::Squads:
			return TEXT("Squads");

		case EAlgonaSpatialGridMode::Soldiers:
			return TEXT("Soldiers");

		case EAlgonaSpatialGridMode::Both:
			return TEXT("Both");
		}

		return TEXT("Unknown");
	}
}

void UAlgonaSimulationSubsystem::RunSimulationStep(
	float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);

	const double StartSeconds =
		FPlatformTime::Seconds();

	++SimulationTick;

	ProcessPendingMoveCommands();

	int32 SquadSpatialCellChanges = 0;

	const bool bSquadAnchorsChanged =
		UpdateSquadAnchors(
			DeltaTime,
			SquadSpatialCellChanges);

	int32 VisitedEntities = 0;
	int32 SoldierSpatialCellChanges = 0;

	const int32 MovedEntities =
		UpdateSoldiers(
			DeltaTime,
			VisitedEntities,
			SoldierSpatialCellChanges);

	if (bSquadAnchorsChanged || MovedEntities > 0)
	{
		++StateRevision;
	}

	const double StepMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	Metrics.SimulationTick = SimulationTick;
	Metrics.EntityCount = SoldierEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = VisitedEntities;
	Metrics.LastMovedEntities = MovedEntities;
	Metrics.LastStepMilliseconds = StepMilliseconds;

	if (!bSpatialGridBenchmarkEnabled)
	{
		return;
	}

	++SpatialBenchmarkStepCount;

	SpatialBenchmarkStepMillisecondsSum +=
		StepMilliseconds;

	SpatialBenchmarkStepMillisecondsMax =
		FMath::Max(
			SpatialBenchmarkStepMillisecondsMax,
			StepMilliseconds);

	SpatialBenchmarkSquadCellChanges +=
		static_cast<uint64>(SquadSpatialCellChanges);

	SpatialBenchmarkSoldierCellChanges +=
		static_cast<uint64>(SoldierSpatialCellChanges);

	if (SpatialBenchmarkStepCount < SpatialBenchmarkWindowSteps)
	{
		return;
	}

	const double AverageStepMilliseconds =
		SpatialBenchmarkStepMillisecondsSum
		/ static_cast<double>(SpatialBenchmarkStepCount);

	const double AverageSquadCellChanges =
		static_cast<double>(SpatialBenchmarkSquadCellChanges)
		/ static_cast<double>(SpatialBenchmarkStepCount);

	const double AverageSoldierCellChanges =
		static_cast<double>(SpatialBenchmarkSoldierCellChanges)
		/ static_cast<double>(SpatialBenchmarkStepCount);

	UE_LOG(
		LogAlgonaSpatialGridBenchmark,
		Display,
		TEXT(
			"Grid benchmark | Mode=%s | Soldiers=%d | Squads=%d | "
			"Cell=%.0f cm | AvgStep=%.3f ms | MaxStep=%.3f ms | "
			"SquadCellChanges=%.1f/step | SoldierCellChanges=%.1f/step"),
		GetSpatialGridModeName(SpatialGridMode),
		SoldierEntities.Num(),
		Squads.Num(),
		AlgonaSimulationDefaults::SpatialGridCellSizeCm,
		AverageStepMilliseconds,
		SpatialBenchmarkStepMillisecondsMax,
		AverageSquadCellChanges,
		AverageSoldierCellChanges);

	SpatialBenchmarkStepCount = 0;
	SpatialBenchmarkStepMillisecondsSum = 0.0;
	SpatialBenchmarkStepMillisecondsMax = 0.0;
	SpatialBenchmarkSquadCellChanges = 0;
	SpatialBenchmarkSoldierCellChanges = 0;
}

void UAlgonaSimulationSubsystem::ProcessPendingMoveCommands()
{
	for (const FAlgonaSquadMoveCommand& Command : PendingMoveCommands)
	{
		if (!Squads.IsValidIndex(Command.SquadId)
			|| Squads[Command.SquadId].SquadId != Command.SquadId)
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Command.SquadId];
		Squad.TargetAnchorLocation = Command.TargetLocation;
		Squad.TargetAnchorLocation.Z = Squad.AnchorLocation.Z;
		Squad.bHasMoveTarget = true;
	}

	PendingMoveCommands.Reset();
}

bool UAlgonaSimulationSubsystem::UpdateSquadAnchors(
	float DeltaTime,
	int32& OutSpatialCellChanges)
{
	OutSpatialCellChanges = 0;

	const bool bUpdateSpatialGrid =
		IsSquadSpatialGridEnabled();

	bool bAnyAnchorChanged = false;

	for (FAlgonaSquad& Squad : Squads)
	{
		if (!Squad.bHasMoveTarget)
		{
			continue;
		}

		const FVector OldSpatialCenter = Squad.GetSpatialCenter();
		const FVector OldAnchorLocation = Squad.AnchorLocation;

		FVector ToTarget =
			Squad.TargetAnchorLocation - Squad.AnchorLocation;
		ToTarget.Z = 0.0;

		const double DistanceToTarget = ToTarget.Length();

		if (DistanceToTarget <= KINDA_SMALL_NUMBER)
		{
			Squad.AnchorLocation = Squad.TargetAnchorLocation;
			Squad.bHasMoveTarget = false;

			if (bUpdateSpatialGrid
				&& SquadSpatialGrid.UpdateSquad(
					Squad.SquadId,
					OldSpatialCenter,
					Squad.GetSpatialCenter()))
			{
				++OutSpatialCellChanges;
			}

			bAnyAnchorChanged |=
				!OldAnchorLocation.Equals(Squad.AnchorLocation, KINDA_SMALL_NUMBER);
			continue;
		}

		const FVector MoveDirection = ToTarget / DistanceToTarget;
		Squad.FacingDirection = MoveDirection;

		const double MaxMoveDistance =
			static_cast<double>(Squad.AnchorMoveSpeed) * DeltaTime;

		if (DistanceToTarget <= MaxMoveDistance)
		{
			Squad.AnchorLocation = Squad.TargetAnchorLocation;
			Squad.bHasMoveTarget = false;
		}
		else
		{
			Squad.AnchorLocation += MoveDirection * MaxMoveDistance;
		}

		if (bUpdateSpatialGrid
			&& SquadSpatialGrid.UpdateSquad(
				Squad.SquadId,
				OldSpatialCenter,
				Squad.GetSpatialCenter()))
		{
			++OutSpatialCellChanges;
		}

		bAnyAnchorChanged = true;
	}

	return bAnyAnchorChanged;
}

int32 UAlgonaSimulationSubsystem::UpdateSoldiers(
	float DeltaTime,
	int32& OutVisitedEntities,
	int32& OutSpatialCellChanges)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSoldiersP1);

	OutVisitedEntities = 0;
	OutSpatialCellChanges = 0;

	const bool bUpdateSpatialGrid =
		IsSoldierSpatialGridEnabled();

	int32 MovedEntities = 0;

	if (!MassEntitySubsystem
		|| !SoldierUpdateQuery
		|| DeltaTime <= 0.0f)
	{
		return 0;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(DeltaTime);

	SoldierUpdateQuery->ForEachEntityChunk(
		ExecutionContext,
		[
			this,
			DeltaTime,
			bUpdateSpatialGrid,
			&OutVisitedEntities,
			&OutSpatialCellChanges,
			&MovedEntities
		]
		(FMassExecutionContext& Context)
		{
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
			const TConstArrayView<FAlgonaSoldierIdFragment> Ids =
				Context.GetFragmentView<FAlgonaSoldierIdFragment>();
			const TConstArrayView<FAlgonaSquadMemberFragment> Members =
				Context.GetFragmentView<FAlgonaSquadMemberFragment>();
			TArrayView<FAlgonaSoldierMovementFragment> Movement =
				Context.GetMutableFragmentView<FAlgonaSoldierMovementFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				++OutVisitedEntities;

				FAlgonaSoldierMovementFragment& SoldierMovement =
					Movement[Index];
				SoldierMovement.LastProcessedSimulationTick = SimulationTick;

				const int32 SquadId = Members[Index].SquadId;
				if (!Squads.IsValidIndex(SquadId)
					|| Squads[SquadId].SquadId != SquadId)
				{
					SoldierMovement.Velocity = FVector::ZeroVector;
					SoldierMovement.State = EAlgonaSoldierMovementState::Idle;
					continue;
				}

				const FAlgonaSquad& Squad = Squads[SquadId];
				FTransform& Transform =
					Transforms[Index].GetMutableTransform();

				const FVector CurrentLocation = Transform.GetLocation();
				const FVector DesiredLocation = ComputeSlotWorldPosition(
					Squad,
					Members[Index].SlotIndex);
				const FVector ToDesiredLocation =
					DesiredLocation - CurrentLocation;

				if (ToDesiredLocation.IsNearlyZero(0.1))
				{
					SoldierMovement.Velocity = FVector::ZeroVector;
					SoldierMovement.State = EAlgonaSoldierMovementState::Idle;
					continue;
				}

				const double MaxMoveDistance =
					static_cast<double>(Squad.SoldierMoveSpeed) * DeltaTime;
				const FVector MoveDelta =
					ToDesiredLocation.GetClampedToMaxSize(MaxMoveDistance);

				SoldierMovement.Velocity = MoveDelta / DeltaTime;
				SoldierMovement.State = EAlgonaSoldierMovementState::Moving;

				const FVector NewLocation =
					CurrentLocation + MoveDelta;

				if (bUpdateSpatialGrid
					&& SoldierSpatialGrid.UpdateSoldier(
						Ids[Index].Value,
						NewLocation))
				{
					++OutSpatialCellChanges;
				}

				Transform.SetLocation(NewLocation);

				if (!SoldierMovement.Velocity.IsNearlyZero())
				{
					Transform.SetRotation(
						SoldierMovement.Velocity.Rotation().Quaternion());
				}

				++MovedEntities;
			}
		});

	return MovedEntities;
}
