#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaSoldierFragments.h"

#include "HAL/PlatformTime.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void UAlgonaSimulationSubsystem::RunSimulationStep(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);
	const double StartSeconds = FPlatformTime::Seconds();

	++SimulationTick;

	ProcessPendingMoveCommands();

	const bool bSquadAnchorsChanged =
		UpdateSquadAnchors(DeltaTime);

	int32 VisitedEntities = 0;
	const int32 MovedEntities =
		UpdateSoldiers(DeltaTime, VisitedEntities);

	// Сейчас snapshots содержат только Transform, поэтому revision
	// увеличиваем только когда могла измениться позиция армии.
	if (bSquadAnchorsChanged || MovedEntities > 0)
	{
		++StateRevision;
	}

	const double StepMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	Metrics.SimulationTick = static_cast<int64>(SimulationTick);
	Metrics.EntityCount = SoldierEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = VisitedEntities;
	Metrics.LastMovedEntities = MovedEntities;
	Metrics.LastStepMilliseconds = StepMilliseconds;
}

void UAlgonaSimulationSubsystem::ProcessPendingMoveCommands()
{
	for (const FAlgonaSquadMoveCommand& Command
		: PendingMoveCommands)
	{
		if (!Squads.IsValidIndex(Command.SquadId)
			|| Squads[Command.SquadId].SquadId
				!= Command.SquadId)
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Command.SquadId];
		Squad.TargetAnchorLocation = Command.TargetLocation;
		Squad.TargetAnchorLocation.Z = Squad.AnchorLocation.Z;
		Squad.bHasMoveTarget = true;
	}

	// Очищаем старые приказы, но память массива можно переиспользовать.
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

		FVector ToTarget =
			Squad.TargetAnchorLocation - Squad.AnchorLocation;
		ToTarget.Z = 0.0;

		const double DistanceToTarget = ToTarget.Length();

		if (DistanceToTarget <= KINDA_SMALL_NUMBER)
		{
			Squad.AnchorLocation = Squad.TargetAnchorLocation;
			Squad.bHasMoveTarget = false;
			continue;
		}

		const FVector MoveDirection =
			ToTarget / DistanceToTarget;

		// Направление обновляем даже если цель достигнется за один step.
		Squad.FacingDirection = MoveDirection;

		const double MaxMoveDistance =
			static_cast<double>(Squad.AnchorMoveSpeed)
			* DeltaTime;

		if (DistanceToTarget <= MaxMoveDistance)
		{
			Squad.AnchorLocation = Squad.TargetAnchorLocation;
			Squad.bHasMoveTarget = false;
			bAnyAnchorChanged = true;
			continue;
		}

		Squad.AnchorLocation +=
			MoveDirection * MaxMoveDistance;
		bAnyAnchorChanged = true;
	}

	return bAnyAnchorChanged;
}

int32 UAlgonaSimulationSubsystem::UpdateSoldiers(
	float DeltaTime,
	int32& OutVisitedEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSoldiersP1);

	OutVisitedEntities = 0;
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
		[this, DeltaTime, &OutVisitedEntities, &MovedEntities](
			FMassExecutionContext& Context)
		{
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();

			const TConstArrayView<FAlgonaSquadMemberFragment> Members =
				Context.GetFragmentView<
					FAlgonaSquadMemberFragment>();

			TArrayView<FAlgonaSoldierMovementFragment> Movement =
				Context.GetMutableFragmentView<
					FAlgonaSoldierMovementFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				++OutVisitedEntities;

				FAlgonaSoldierMovementFragment& SoldierMovement =
					Movement[Index];
				SoldierMovement.LastProcessedSimulationTick =
					SimulationTick;

				const int32 SquadId = Members[Index].SquadId;

				if (!Squads.IsValidIndex(SquadId)
					|| Squads[SquadId].SquadId != SquadId)
				{
					SoldierMovement.Velocity = FVector::ZeroVector;
					SoldierMovement.State =
						EAlgonaSoldierMovementState::Idle;
					continue;
				}

				const FAlgonaSquad& Squad = Squads[SquadId];
				FTransform& Transform =
					Transforms[Index].GetMutableTransform();

				const FVector CurrentLocation =
					Transform.GetLocation();
				const FVector DesiredLocation =
					ComputeSlotWorldPosition(
						Squad,
						Members[Index].SlotIndex);

				const FVector ToDesiredLocation =
					DesiredLocation - CurrentLocation;

				if (ToDesiredLocation.IsNearlyZero(0.1))
				{
					SoldierMovement.Velocity =
						FVector::ZeroVector;
					SoldierMovement.State =
						EAlgonaSoldierMovementState::Idle;
					continue;
				}

				const double MaxMoveDistance =
					static_cast<double>(Squad.SoldierMoveSpeed)
					* DeltaTime;

				const FVector MoveDelta =
					ToDesiredLocation.GetClampedToMaxSize(
						MaxMoveDistance);

				SoldierMovement.Velocity =
					MoveDelta / DeltaTime;
				SoldierMovement.State =
					EAlgonaSoldierMovementState::Moving;

				Transform.SetLocation(
					CurrentLocation + MoveDelta);

				if (!SoldierMovement.Velocity.IsNearlyZero())
				{
					Transform.SetRotation(
						SoldierMovement
							.Velocity
							.Rotation()
							.Quaternion());
				}

				++MovedEntities;
			}
		});

	return MovedEntities;
}