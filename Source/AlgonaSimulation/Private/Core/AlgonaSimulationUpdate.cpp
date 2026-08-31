#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"

#include "HAL/PlatformTime.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void UAlgonaSimulationSubsystem::RunSimulationStep(
	float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);

	const double StartSeconds =
		FPlatformTime::Seconds();

	++SimulationTick;

	ProcessPendingMoveCommands();

	const bool bSquadAnchorsChanged =
		UpdateSquadAnchors(DeltaTime);

	int32 VisitedEntities = 0;
	const int32 MovedEntities =
		UpdateUnits(
			DeltaTime,
			VisitedEntities);

	if (bSquadAnchorsChanged || MovedEntities > 0)
	{
		++StateRevision;
	}

	const double StepMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	Metrics.SimulationTick = SimulationTick;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = VisitedEntities;
	Metrics.LastMovedEntities = MovedEntities;
	Metrics.LastStepMilliseconds = StepMilliseconds;
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
	float DeltaTime)
{
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

			if (IsSquadSpatialGridEnabled())
			{
				SquadSpatialGrid.UpdateSquad(
					Squad.SquadId,
					OldSpatialCenter,
					Squad.GetSpatialCenter());
			}

			bAnyAnchorChanged |=
				!OldAnchorLocation.Equals(
					Squad.AnchorLocation,
					KINDA_SMALL_NUMBER);
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

		if (IsSquadSpatialGridEnabled())
		{
			SquadSpatialGrid.UpdateSquad(
				Squad.SquadId,
				OldSpatialCenter,
				Squad.GetSpatialCenter());
		}

		bAnyAnchorChanged = true;
	}

	return bAnyAnchorChanged;
}

int32 UAlgonaSimulationSubsystem::UpdateUnits(
	float DeltaTime,
	int32& OutVisitedEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateUnitsP1);

	OutVisitedEntities = 0;
	int32 MovedEntities = 0;

	if (!MassEntitySubsystem
		|| !UnitUpdateQuery
		|| DeltaTime <= 0.0f)
	{
		return 0;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(DeltaTime);

	UnitUpdateQuery->ForEachEntityChunk(
		ExecutionContext,
		[
			this,
			DeltaTime,
			&OutVisitedEntities,
			&MovedEntities
		](FMassExecutionContext& Context)
		{
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();
			const TConstArrayView<FAlgonaSquadMemberFragment> Members =
				Context.GetFragmentView<FAlgonaSquadMemberFragment>();
			TArrayView<FAlgonaUnitMovementFragment> Movement =
				Context.GetMutableFragmentView<FAlgonaUnitMovementFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				++OutVisitedEntities;

				FAlgonaUnitMovementFragment& UnitMovement =
					Movement[Index];
				UnitMovement.LastProcessedSimulationTick = SimulationTick;

				const int32 SquadId = Members[Index].SquadId;
				if (!Squads.IsValidIndex(SquadId)
					|| Squads[SquadId].SquadId != SquadId)
				{
					UnitMovement.Velocity = FVector::ZeroVector;
					UnitMovement.State = EAlgonaUnitMovementState::Idle;
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
					UnitMovement.Velocity = FVector::ZeroVector;
					UnitMovement.State = EAlgonaUnitMovementState::Idle;
					continue;
				}

				const double MaxMoveDistance =
					static_cast<double>(Squad.UnitMoveSpeed) * DeltaTime;
				const FVector MoveDelta =
					ToDesiredLocation.GetClampedToMaxSize(MaxMoveDistance);

				UnitMovement.Velocity = MoveDelta / DeltaTime;
				UnitMovement.State = EAlgonaUnitMovementState::Moving;

				const FVector NewLocation =
					CurrentLocation + MoveDelta;

				UnitSpatialGrid.UpdateUnit(
					Ids[Index].Value,
					NewLocation);

				Transform.SetLocation(NewLocation);

				if (!UnitMovement.Velocity.IsNearlyZero())
				{
					Transform.SetRotation(
						UnitMovement.Velocity.Rotation().Quaternion());
				}

				++MovedEntities;
			}
		});

	return MovedEntities;
}
