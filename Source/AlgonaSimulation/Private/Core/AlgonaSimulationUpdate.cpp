#include "Core/AlgonaSimulationSubsystem.h"

// Authoritative soldier state updated inside the fixed-step Mass pass.
#include "Army/AlgonaSoldierFragments.h"

// UE 5.8 navigation API: one synchronous path request per squad command.
#include "NavigationPath.h"
#include "NavigationSystem.h"

// Mass execution and profiling infrastructure.
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	constexpr double RoutePointToleranceCm = 1.0;
	constexpr double TurnCompletionToleranceRadians = 0.004363323129985824;
	constexpr double SoldierBodyTurnSpeedRadiansPerSecond = 5.235987755982989;
	constexpr float TravelDriftMinSegmentLengthCm = 1500.0f;
	constexpr float TravelDriftMaxSegmentLengthCm = 3000.0f;
	constexpr float TravelDriftMaxSideOffsetCm = 40.0f;
	constexpr double FollowCatchupPerSecond = 1.5;
	constexpr double ReformStartDistanceCm = 1500.0;
	constexpr double ReformCompletionToleranceCm = 1.0;

	double SignedAngleRadians2D(const FVector& From, const FVector& To)
	{
		const FVector SafeFrom = From.GetSafeNormal2D();
		const FVector SafeTo = To.GetSafeNormal2D();
		if (SafeFrom.IsNearlyZero() || SafeTo.IsNearlyZero())
		{
			return 0.0;
		}

		const double Dot = FMath::Clamp(
			static_cast<double>(FVector::DotProduct(SafeFrom, SafeTo)),
			-1.0,
			1.0);
		const double CrossZ = static_cast<double>(
			FVector::CrossProduct(SafeFrom, SafeTo).Z);
		return FMath::Atan2(CrossZ, Dot);
	}

	FVector RotateDirection2D(
		const FVector& CurrentDirection,
		double SignedAngleRadians)
	{
		FVector SafeDirection = CurrentDirection.GetSafeNormal2D();
		if (SafeDirection.IsNearlyZero())
		{
			SafeDirection = FVector::ForwardVector;
		}

		const FQuat Rotation(
			FVector::UpVector,
			static_cast<float>(SignedAngleRadians));
		return Rotation.RotateVector(SafeDirection).GetSafeNormal2D();
	}

	uint32 HashMotionValue(uint32 Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7feb352du;
		Value ^= Value >> 15;
		Value *= 0x846ca68bu;
		Value ^= Value >> 16;
		return Value;
	}

	float HashMotionUnit(uint32 Value)
	{
		return static_cast<float>(HashMotionValue(Value) & 0x00ffffffu)
			/ static_cast<float>(0x01000000u);
	}

	void EnsureCommandMotionVariation(
		const FAlgonaSquad& Squad,
		FAlgonaSoldierMovementFragment& Movement)
	{
		if (Movement.CommandVariationRevision == Squad.MoveCommandRevision)
		{
			return;
		}

		const uint32 CommandSeed = Movement.MotionVariationSeed
			^ (Squad.MoveCommandRevision * 0x9e3779b9u);

		Movement.TravelDriftSegmentLengthCm = FMath::Lerp(
			TravelDriftMinSegmentLengthCm,
			TravelDriftMaxSegmentLengthCm,
			HashMotionUnit(CommandSeed ^ 0x68bc21ebu));

		// Preserve the exact offset reached by the previous command. Segment zero
		// of this command starts here, so accepting a new order cannot snap the
		// soldier back to its canonical Slot.
		Movement.CommandStartSideOffsetCm = FMath::Clamp(
			Movement.CurrentSideOffsetCm,
			-TravelDriftMaxSideOffsetCm,
			TravelDriftMaxSideOffsetCm);
		Movement.CommandVariationRevision = Squad.MoveCommandRevision;
	}

	float ComputeTravelDriftKnotOffsetCm(
		const FAlgonaSquad& Squad,
		const FAlgonaSoldierMovementFragment& Movement,
		int32 KnotIndex)
	{
		// Segment zero starts from the soldier's already existing offset. Later
		// knots are deterministic signed targets, so the next command continues
		// from the previous resting position without any reset.
		if (KnotIndex <= 0)
		{
			return Movement.CommandStartSideOffsetCm;
		}

		const uint32 CommandSeed = Movement.MotionVariationSeed
			^ (Squad.MoveCommandRevision * 0x9e3779b9u);
		const uint32 KnotSeed = CommandSeed
			^ (static_cast<uint32>(KnotIndex) * 0x27d4eb2du)
			^ 0x165667b1u;

		return FMath::Lerp(
			-TravelDriftMaxSideOffsetCm,
			TravelDriftMaxSideOffsetCm,
			HashMotionUnit(KnotSeed));
	}

	FVector ComputePersonalMovementOffset(
		const FAlgonaSquad& Squad,
		FAlgonaSoldierMovementFragment& Movement)
	{
		// Personal lateral drift belongs to the formation frame, not to the
		// soldier body. If the offset rotated with the body while the body faced
		// velocity, turning would move the target itself and create a feedback loop.
		FVector FormationForward = Squad.FacingDirection.GetSafeNormal2D();
		if (FormationForward.IsNearlyZero())
		{
			FormationForward = FVector::ForwardVector;
		}

		FVector FormationRight = FVector::CrossProduct(
			FVector::UpVector,
			FormationForward).GetSafeNormal2D();
		if (FormationRight.IsNearlyZero())
		{
			FormationRight = FVector::RightVector;
		}

		if (!Squad.bHasMoveTarget)
		{
			// Idle soldiers keep the exact lateral imperfection reached while moving.
			// There is no separate final offset and therefore nothing can stack on top.
			Movement.CurrentSideOffsetCm = FMath::Clamp(
				Movement.CurrentSideOffsetCm,
				-TravelDriftMaxSideOffsetCm,
				TravelDriftMaxSideOffsetCm);
			return FormationRight * static_cast<double>(Movement.CurrentSideOffsetCm);
		}

		EnsureCommandMotionVariation(Squad, Movement);

		// Travel drift is keyed to literal distance travelled by the squad center.
		// Each soldier gets a stable 15-30 m interval for the current command.
		// SmoothStep interpolation between fixed signed knots keeps the trajectory
		// gently curved without per-tick skating. The current value is stored back
		// into the soldier and survives after the command finishes.
		const double SegmentLengthCm = FMath::Clamp(
			static_cast<double>(Movement.TravelDriftSegmentLengthCm),
			static_cast<double>(TravelDriftMinSegmentLengthCm),
			static_cast<double>(TravelDriftMaxSegmentLengthCm));
		const double TravelDistanceCm = FMath::Max(
			Squad.CommandTravelDistanceCm,
			0.0);
		const int32 SegmentIndex = FMath::Max(
			0,
			FMath::FloorToInt(TravelDistanceCm / SegmentLengthCm));
		const double SegmentStartCm =
			static_cast<double>(SegmentIndex) * SegmentLengthCm;
		const float SegmentAlpha = FMath::Clamp(
			static_cast<float>((TravelDistanceCm - SegmentStartCm) / SegmentLengthCm),
			0.0f,
			1.0f);
		const float SmoothAlpha =
			SegmentAlpha * SegmentAlpha * (3.0f - 2.0f * SegmentAlpha);

		const float SegmentStartOffsetCm = ComputeTravelDriftKnotOffsetCm(
			Squad,
			Movement,
			SegmentIndex);
		const float SegmentEndOffsetCm = ComputeTravelDriftKnotOffsetCm(
			Squad,
			Movement,
			SegmentIndex + 1);
		Movement.CurrentSideOffsetCm = FMath::Clamp(
			FMath::Lerp(
				SegmentStartOffsetCm,
				SegmentEndOffsetCm,
				SmoothAlpha),
			-TravelDriftMaxSideOffsetCm,
			TravelDriftMaxSideOffsetCm);

		return FormationRight * static_cast<double>(Movement.CurrentSideOffsetCm);
	}

	double ComputeRouteLength2D(TConstArrayView<FVector> RoutePoints)
	{
		double TotalLengthCm = 0.0;
		for (int32 Index = 1; Index < RoutePoints.Num(); ++Index)
		{
			TotalLengthCm += (
				RoutePoints[Index] - RoutePoints[Index - 1]).Size2D();
		}
		return TotalLengthCm;
	}

	double ComputeRemainingRouteDistance2D(const FAlgonaSquad& Squad)
	{
		if (!Squad.RoutePoints.IsValidIndex(Squad.NextRoutePointIndex))
		{
			return (Squad.FinalTargetLocation - Squad.AnchorLocation).Size2D();
		}

		double RemainingDistanceCm = (
			Squad.RoutePoints[Squad.NextRoutePointIndex] - Squad.AnchorLocation)
			.Size2D();

		for (int32 Index = Squad.NextRoutePointIndex + 1;
			Index < Squad.RoutePoints.Num();
			++Index)
		{
			RemainingDistanceCm += (
				Squad.RoutePoints[Index] - Squad.RoutePoints[Index - 1]).Size2D();
		}

		return RemainingDistanceCm;
	}

	double ComputeDistanceOutsideFormationSquared2D(
		const FAlgonaSquad& Squad,
		const FVector& WorldLocation)
	{
		FVector Forward = Squad.FacingDirection.GetSafeNormal2D();
		if (Forward.IsNearlyZero())
		{
			Forward = FVector::ForwardVector;
		}

		FVector Right = FVector::CrossProduct(
			FVector::UpVector,
			Forward).GetSafeNormal2D();
		if (Right.IsNearlyZero())
		{
			Right = FVector::RightVector;
		}

		const FVector FromCenter = WorldLocation - Squad.AnchorLocation;
		const double LocalX = FVector::DotProduct(FromCenter, Right);
		const double LocalY = FVector::DotProduct(FromCenter, Forward);
		const double OutsideX = FMath::Max(
			FMath::Abs(LocalX) - static_cast<double>(Squad.Formation.HalfExtentsCm.X),
			0.0);
		const double OutsideY = FMath::Max(
			FMath::Abs(LocalY) - static_cast<double>(Squad.Formation.HalfExtentsCm.Y),
			0.0);

		return OutsideX * OutsideX + OutsideY * OutsideY;
	}

	FQuat RotateSoldierBodyToward(
		const FQuat& CurrentRotation,
		const FVector& DesiredFacing,
		float TurnSpeedScale,
		uint32 VariationSeed,
		float DeltaTime)
	{
		FVector TargetFacing = DesiredFacing.GetSafeNormal2D();
		if (TargetFacing.IsNearlyZero() || DeltaTime <= 0.0f)
		{
			return CurrentRotation;
		}

		FVector CurrentFacing = CurrentRotation
			.RotateVector(FVector::ForwardVector)
			.GetSafeNormal2D();
		if (CurrentFacing.IsNearlyZero())
		{
			CurrentFacing = FVector::ForwardVector;
		}

		double SignedTurn = SignedAngleRadians2D(CurrentFacing, TargetFacing);

		// Exactly opposite bodies have two equally short turn directions. Splitting
		// them deterministically prevents the entire rank from pirouetting in sync.
		if (FMath::IsNearlyEqual(FMath::Abs(SignedTurn), PI, 1.0e-4))
		{
			SignedTurn = (VariationSeed & 1u) != 0u ? PI : -PI;
		}

		const double MaxTurn =
			SoldierBodyTurnSpeedRadiansPerSecond
			* FMath::Max(0.1f, TurnSpeedScale)
			* static_cast<double>(DeltaTime);
		const double AppliedTurn = FMath::Clamp(SignedTurn, -MaxTurn, MaxTurn);

		if (FMath::Abs(SignedTurn) <= TurnCompletionToleranceRadians)
		{
			return TargetFacing.Rotation().Quaternion();
		}

		const FVector NewFacing = RotateDirection2D(CurrentFacing, AppliedTurn);
		return NewFacing.Rotation().Quaternion();
	}

	bool BuildRouteForCommand(
		UWorld& World,
		const FVector& StartLocation,
		const FVector& ExactTargetLocation,
		TArray<FVector>& OutRoutePoints)
	{
		OutRoutePoints.Reset();

		UNavigationSystemV1* NavigationSystem =
			UNavigationSystemV1::GetCurrent(&World);

		// The existing P0/P1 test map has historically worked without a NavMesh.
		// Keep that map usable, but use the direct fallback only when there is no
		// default navigation data at all. A real pathfinding failure is not
		// allowed to turn silently into movement through world geometry.
		if (!NavigationSystem
			|| NavigationSystem->GetDefaultNavDataInstance() == nullptr)
		{
			OutRoutePoints.Add(StartLocation);
			OutRoutePoints.Add(ExactTargetLocation);
			return true;
		}

		UNavigationPath* NavigationPath =
			UNavigationSystemV1::FindPathToLocationSynchronously(
				&World,
				StartLocation,
				ExactTargetLocation,
				nullptr,
				nullptr);

		if (!NavigationPath
			|| !NavigationPath->IsValid()
			|| NavigationPath->IsPartial()
			|| NavigationPath->PathPoints.Num() < 2)
		{
			return false;
		}

		OutRoutePoints = NavigationPath->PathPoints;

		// NavMesh can project the endpoint. Gameplay semantics require the squad
		// center itself to finish at the player's exact requested point.
		if (OutRoutePoints.Last().Equals(ExactTargetLocation, 0.1))
		{
			OutRoutePoints.Last() = ExactTargetLocation;
		}
		else
		{
			OutRoutePoints.Add(ExactTargetLocation);
		}

		return true;
	}

	void FinishSquadMoveCommand(FAlgonaSquad& Squad)
	{
		Squad.bHasMoveTarget = false;
		Squad.bCenterAtDestination = false;
		Squad.bWaitingForStragglers = false;
		Squad.StragglerWaitRemainingSeconds = 0.0f;
		Squad.CohesionMoveScale = 1.0f;
		Squad.FinalAssemblyRemainingSeconds = 0.0f;
		Squad.RoutePoints.Reset();
		Squad.NextRoutePointIndex = INDEX_NONE;
		Squad.bHasExplicitFinalFacing = false;
		Squad.bFinalFacingPending = false;
		Squad.bReformPending = false;
		Squad.PendingReformMaxSlotsPerRow = 0;
		// Per-soldier side offsets remain stored in their movement fragments while
		// idle, so the next command continues from the same imperfect formation.
	}
}

void UAlgonaSimulationSubsystem::RunSimulationStep(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);
	const double StartSeconds = FPlatformTime::Seconds();

	++SimulationTick;
	ProcessPendingMoveCommands();

	const bool bSquadAnchorsChanged = UpdateSquadAnchors(DeltaTime);

	int32 VisitedEntities = 0;
	const int32 ChangedEntities = UpdateSoldiers(DeltaTime, VisitedEntities);

	// Cohesion uses counters filled in the same soldier pass; no second Mass scan.
	EvaluateSquadCohesion(DeltaTime);

	// Presentation snapshots expose transforms only. Revision therefore changes
	// when a center moved/turned or at least one soldier transform changed.
	if (bSquadAnchorsChanged || ChangedEntities > 0)
	{
		++StateRevision;
	}

	Metrics.SimulationTick = SimulationTick;
	Metrics.EntityCount = SoldierEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = VisitedEntities;
	Metrics.LastMovedEntities = ChangedEntities;
	Metrics.LastStepMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
}

void UAlgonaSimulationSubsystem::ProcessPendingMoveCommands()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		PendingMoveCommands.Reset();
		return;
	}

	for (const FAlgonaSquadMoveCommand& Command : PendingMoveCommands)
	{
		if (!Squads.IsValidIndex(Command.SquadId)
			|| Squads[Command.SquadId].SquadId != Command.SquadId)
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Command.SquadId];
		TArray<FVector> NewRoutePoints;
		if (!BuildRouteForCommand(
			*World,
			Squad.AnchorLocation,
			Command.TargetLocation,
			NewRoutePoints))
		{
			// A real navigation failure rejects the new order without disturbing the
			// currently running/settled squad state.
			continue;
		}

		// A new accepted command replaces the previous route atomically at this
		// fixed step. Any older held-RMB Reform that has not started is invalid now.
		Squad.bReformPending = false;
		Squad.PendingReformMaxSlotsPerRow = 0;
		Squad.MovePace = Command.Pace;
		Squad.FinalTargetLocation = Command.TargetLocation;
		Squad.RoutePoints = MoveTemp(NewRoutePoints);
		Squad.NextRoutePointIndex = INDEX_NONE;
		Squad.bHasMoveTarget = false;
		Squad.bCenterAtDestination = false;
		Squad.bWaitingForStragglers = false;
		Squad.StragglerWaitRemainingSeconds = 0.0f;
		Squad.CohesionMoveScale = 1.0f;
		Squad.FinalAssemblyRemainingSeconds = 0.0f;
		Squad.bFinalFacingPending = false;
		Squad.CommandRouteLengthCm = ComputeRouteLength2D(Squad.RoutePoints);
		Squad.CommandTravelDistanceCm = 0.0;
		++Squad.MoveCommandRevision;
		if (Squad.MoveCommandRevision == 0)
		{
			++Squad.MoveCommandRevision;
		}

		const FVector ExplicitFinalFacing =
			Command.FinalFacingDirection.GetSafeNormal2D();
		Squad.bHasExplicitFinalFacing = !ExplicitFinalFacing.IsNearlyZero();
		if (Squad.bHasExplicitFinalFacing)
		{
			Squad.FinalFacingDirection = ExplicitFinalFacing;
		}

		Squad.NextRoutePointIndex =
			Squad.RoutePoints.Num() > 1 ? 1 : 0;

		if (Command.RequestedMaxSlotsPerRow > 0
			&& !Squad.ActiveMemberSoldierIndices.IsEmpty())
		{
			const int32 NewMaxSlotsPerRow = FMath::Clamp(
				Command.RequestedMaxSlotsPerRow,
				1,
				Squad.ActiveMemberSoldierIndices.Num());

			if (Squad.Formation.MaxSlotsPerRow != NewMaxSlotsPerRow)
			{
				Squad.bReformPending = true;
				Squad.PendingReformMaxSlotsPerRow = NewMaxSlotsPerRow;

				// A short held-RMB route starts Reform immediately. A longer one
				// keeps the current formation until only 15 m of route remain.
				if (ComputeRemainingRouteDistance2D(Squad) <= ReformStartDistanceCm)
				{
					Squad.RequestedMaxSlotsPerRow = NewMaxSlotsPerRow;
					ReformSquad(Squad);
					Squad.bReformPending = false;
					Squad.PendingReformMaxSlotsPerRow = 0;
					Squad.bReformInProgress = true;
					Squad.bWaitingForStragglers = false;
					Squad.StragglerWaitRemainingSeconds = 0.0f;
					Squad.CohesionMoveScale = 1.0f;
				}
			}
		}

		Squad.bHasMoveTarget = true;
	}

	PendingMoveCommands.Reset();
}

bool UAlgonaSimulationSubsystem::UpdateSquadAnchors(float DeltaTime)
{
	bool bAnySquadTransformChanged = false;

	if (DeltaTime <= 0.0f)
	{
		return false;
	}

	for (FAlgonaSquad& Squad : Squads)
	{
		if (!Squad.bHasMoveTarget || Squad.RoutePoints.IsEmpty())
		{
			continue;
		}

		// Held-RMB Reform is a near-destination operation. Until the final 15 m
		// the squad keeps its old shape; a newer accepted order can cancel this
		// pending request before any formation geometry changes.
		if (Squad.bReformPending
			&& Squad.PendingReformMaxSlotsPerRow > 0
			&& ComputeRemainingRouteDistance2D(Squad) <= ReformStartDistanceCm)
		{
			Squad.RequestedMaxSlotsPerRow = Squad.PendingReformMaxSlotsPerRow;
			ReformSquad(Squad);
			Squad.bReformPending = false;
			Squad.PendingReformMaxSlotsPerRow = 0;
			Squad.bReformInProgress = true;

			// Reform itself must not manufacture stragglers. Resume full center
			// speed and suspend cohesion until members settle into the new shape.
			Squad.bWaitingForStragglers = false;
			Squad.StragglerWaitRemainingSeconds = 0.0f;
			Squad.CohesionMoveScale = 1.0f;
		}

		// The center is already fixed at the exact target. A held-RMB command may
		// still need to turn the logical formation toward its requested final facing.
		if (Squad.bCenterAtDestination)
		{
			if (!Squad.bFinalFacingPending)
			{
				continue;
			}

			FVector DesiredFacing = Squad.FinalFacingDirection.GetSafeNormal2D();
			FVector CurrentFacing = Squad.FacingDirection.GetSafeNormal2D();
			if (DesiredFacing.IsNearlyZero())
			{
				Squad.bFinalFacingPending = false;
				continue;
			}
			if (CurrentFacing.IsNearlyZero())
			{
				CurrentFacing = FVector::ForwardVector;
				Squad.FacingDirection = CurrentFacing;
			}

			double SignedTurnRadians =
				SignedAngleRadians2D(CurrentFacing, DesiredFacing);
			if (FMath::Abs(SignedTurnRadians) > (PI * 0.5))
			{
				MirrorActiveMemberAssignments(Squad);
				Squad.FacingDirection = -CurrentFacing;
				CurrentFacing = Squad.FacingDirection;
				SignedTurnRadians =
					SignedAngleRadians2D(CurrentFacing, DesiredFacing);
				bAnySquadTransformChanged = true;
			}

			const double AbsoluteTurnRadians = FMath::Abs(SignedTurnRadians);
			if (AbsoluteTurnRadians <= TurnCompletionToleranceRadians)
			{
				Squad.FacingDirection = DesiredFacing;
				Squad.bFinalFacingPending = false;
				continue;
			}

			const double SoldierSpeedLimit =
				static_cast<double>(Squad.GetActiveSoldierSpeedLimit());
			const double MaxAngularSpeedRadians =
				Squad.Formation.RadiusCm > KINDA_SMALL_NUMBER
					? SoldierSpeedLimit
						/ static_cast<double>(Squad.Formation.RadiusCm)
					: TNumericLimits<double>::Max();

			if (MaxAngularSpeedRadians <= SMALL_NUMBER)
			{
				continue;
			}

			const double AppliedTurnRadians = FMath::Min(
				AbsoluteTurnRadians,
				MaxAngularSpeedRadians * static_cast<double>(DeltaTime));
			Squad.FacingDirection = RotateDirection2D(
				Squad.FacingDirection,
				FMath::Sign(SignedTurnRadians) * AppliedTurnRadians);
			bAnySquadTransformChanged |= AppliedTurnRadians > SMALL_NUMBER;

			if (AbsoluteTurnRadians - AppliedTurnRadians
				<= TurnCompletionToleranceRadians)
			{
				Squad.FacingDirection = DesiredFacing;
				Squad.bFinalFacingPending = false;
			}
			continue;
		}

		if (Squad.bWaitingForStragglers)
		{
			continue;
		}

		// Skip path points already reached by the geometric center.
		while (Squad.RoutePoints.IsValidIndex(Squad.NextRoutePointIndex)
			&& FVector::DistSquared(
				Squad.AnchorLocation,
				Squad.RoutePoints[Squad.NextRoutePointIndex])
				<= FMath::Square(RoutePointToleranceCm))
		{
			++Squad.NextRoutePointIndex;
		}

		if (!Squad.RoutePoints.IsValidIndex(Squad.NextRoutePointIndex))
		{
			const FVector OldCenter = Squad.AnchorLocation;
			Squad.AnchorLocation = Squad.FinalTargetLocation;
			Squad.CommandTravelDistanceCm +=
				(Squad.AnchorLocation - OldCenter).Size2D();

			if (!OldCenter.Equals(Squad.AnchorLocation, 0.01))
			{
				SquadSpatialGrid.UpdateSquad(
					Squad.SquadId,
					OldCenter,
					Squad.AnchorLocation);
				bAnySquadTransformChanged = true;
			}


			Squad.bCenterAtDestination = true;
			Squad.bFinalFacingPending = Squad.bHasExplicitFinalFacing;
			Squad.FinalAssemblyRemainingSeconds =
				AlgonaSimulationDefaults::FinalAssemblyTimeoutSeconds + DeltaTime;
			continue;
		}

		const FVector RouteTarget =
			Squad.RoutePoints[Squad.NextRoutePointIndex];
		const FVector ToRouteTarget = RouteTarget - Squad.AnchorLocation;
		const FVector DesiredFacing = ToRouteTarget.GetSafeNormal2D();

		if (DesiredFacing.IsNearlyZero())
		{
			continue;
		}

		FVector CurrentFacing = Squad.FacingDirection.GetSafeNormal2D();
		if (CurrentFacing.IsNearlyZero())
		{
			CurrentFacing = FVector::ForwardVector;
			Squad.FacingDirection = CurrentFacing;
		}

		double SignedTurnRadians =
			SignedAngleRadians2D(CurrentFacing, DesiredFacing);

		// Preserve the original >90 degree about-face path: mirror slot
		// assignments and flip only the logical formation immediately. Soldier
		// bodies still rotate smoothly, while ordinary <=90 turns use the arc below.
		if (FMath::Abs(SignedTurnRadians) > (PI * 0.5))
		{
			MirrorActiveMemberAssignments(Squad);
			Squad.FacingDirection = -CurrentFacing;
			CurrentFacing = Squad.FacingDirection;
			SignedTurnRadians =
				SignedAngleRadians2D(CurrentFacing, DesiredFacing);
			bAnySquadTransformChanged = true;
		}

		const double AbsoluteTurnRadians = FMath::Abs(SignedTurnRadians);
		const double DistanceToRouteTarget = ToRouteTarget.Size2D();
		const bool bNeedsTurn =
			AbsoluteTurnRadians > TurnCompletionToleranceRadians;
		const bool bNeedsMove =
			DistanceToRouteTarget > RoutePointToleranceCm;

		const double SoldierSpeedLimit =
			static_cast<double>(Squad.GetActiveSoldierSpeedLimit());
		const double CenterSpeed =
			static_cast<double>(Squad.GetCenterMoveSpeed());

		// The formation keeps its existing radius-based angular limit, but center
		// translation no longer loses a time share while turning. Soldiers remain
		// capped independently and may naturally lag a little on a hard wheel turn.
		const double MaxAngularSpeedRadians =
			Squad.Formation.RadiusCm > KINDA_SMALL_NUMBER
				? SoldierSpeedLimit
					/ static_cast<double>(Squad.Formation.RadiusCm)
				: TNumericLimits<double>::Max();

		if (bNeedsTurn && MaxAngularSpeedRadians > SMALL_NUMBER)
		{
			const double AppliedTurnRadians = FMath::Min(
				AbsoluteTurnRadians,
				MaxAngularSpeedRadians * static_cast<double>(DeltaTime));
			const double SignedAppliedTurn =
				FMath::Sign(SignedTurnRadians) * AppliedTurnRadians;

			Squad.FacingDirection = RotateDirection2D(
				Squad.FacingDirection,
				SignedAppliedTurn);
			bAnySquadTransformChanged |= AppliedTurnRadians > SMALL_NUMBER;
		}
		else if (!bNeedsTurn)
		{
			Squad.FacingDirection = DesiredFacing;
		}

		if (!bNeedsMove || CenterSpeed <= SMALL_NUMBER)
		{
			continue;
		}

		const FVector OldCenter = Squad.AnchorLocation;
		const double MaxMoveDistance =
			CenterSpeed * static_cast<double>(DeltaTime);

		// Move along the gradually turning formation heading instead of translating
		// straight at the waypoint while the rectangle spins around its center. This
		// produces a real wheel-turn arc. Inside the last six meters, blend back
		// toward the waypoint so the bounded-curvature prototype cannot orbit it.
		FVector ArcMoveDirection = Squad.FacingDirection.GetSafeNormal2D();
		if (ArcMoveDirection.IsNearlyZero())
		{
			ArcMoveDirection = DesiredFacing;
		}
		constexpr double ArrivalBlendDistanceCm = 600.0;
		const double DirectWeight = FMath::Clamp(
			1.0 - DistanceToRouteTarget / ArrivalBlendDistanceCm,
			0.0,
			1.0);
		FVector MoveDirection = (
			ArcMoveDirection * (1.0 - DirectWeight)
			+ DesiredFacing * DirectWeight).GetSafeNormal2D();
		if (MoveDirection.IsNearlyZero())
		{
			MoveDirection = DesiredFacing;
		}

		FVector MoveDelta = MoveDirection * MaxMoveDistance;
		if (DistanceToRouteTarget <= MaxMoveDistance)
		{
			MoveDelta = ToRouteTarget;
		}
		Squad.AnchorLocation += MoveDelta;

		// This is literal distance travelled by the center, not route completion
		// percentage or progress toward the current waypoint. Curved steering must
		// therefore advance personal drift at the same metres-per-segment cadence.
		Squad.CommandTravelDistanceCm += MoveDelta.Size2D();

		if (!MoveDelta.IsNearlyZero())
		{
			SquadSpatialGrid.UpdateSquad(
				Squad.SquadId,
				OldCenter,
				Squad.AnchorLocation);
			bAnySquadTransformChanged = true;
		}

		// Intermediate points advance on the next fixed step. The final point remains
		// exact; held-RMB width may have Reformed during the final 15 m.
		if (FVector::DistSquared(Squad.AnchorLocation, RouteTarget)
			<= FMath::Square(RoutePointToleranceCm))
		{
			Squad.AnchorLocation = RouteTarget;
			++Squad.NextRoutePointIndex;
		}
	}

	return bAnySquadTransformChanged;
}

int32 UAlgonaSimulationSubsystem::UpdateSoldiers(
	float DeltaTime,
	int32& OutVisitedEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSoldiersP2);

	OutVisitedEntities = 0;
	int32 ChangedEntities = 0;
	RecoveredLostSoldierIndices.Reset();

	if (!MassEntitySubsystem
		|| !SoldierUpdateQuery
		|| DeltaTime <= 0.0f)
	{
		return 0;
	}

	for (FAlgonaSquad& Squad : Squads)
	{
		Squad.DistanceCounters.Reset();
		Squad.ReformUnsettledActiveCount = 0;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(DeltaTime);

	// One Mass pass both moves all soldiers and fills Near/Medium/Far counters.
	SoldierUpdateQuery->ForEachEntityChunk(
		ExecutionContext,
		[this, DeltaTime, &OutVisitedEntities, &ChangedEntities](
			FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaSoldierIdFragment> Ids =
				Context.GetFragmentView<FAlgonaSoldierIdFragment>();
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
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

				const FAlgonaSquadMemberFragment& Member = Members[Index];
				if (!Squads.IsValidIndex(Member.SquadId)
					|| Squads[Member.SquadId].SquadId != Member.SquadId)
				{
					SoldierMovement.Velocity = FVector::ZeroVector;
					SoldierMovement.State = EAlgonaSoldierMovementState::Idle;
					continue;
				}

				FAlgonaSquad& Squad = Squads[Member.SquadId];
				FTransform& Transform =
					Transforms[Index].GetMutableTransform();
				const FVector OldLocation = Transform.GetLocation();
				const FQuat OldRotation = Transform.GetRotation();

				if (Member.FormationState == EAlgonaSoldierFormationState::Lost)
				{
					// First-pass Lost recovery follows the squad center directly. It
					// intentionally ignores unit-to-unit avoidance; world-obstacle pathing
					// is a separate P2 integration step once fixed-step Mass Avoidance is
					// proven for UE 5.8.
					const FVector ToCenter = Squad.AnchorLocation - OldLocation;
					const double MaxMoveDistance =
						static_cast<double>(Squad.GetLostSoldierReturnSpeedLimit())
						* DeltaTime;
					const FVector MoveDelta =
						ToCenter.GetClampedToMaxSize(MaxMoveDistance);

					const FVector NewLocation = OldLocation + MoveDelta;
					Transform.SetLocation(NewLocation);
					SoldierMovement.Velocity = MoveDelta / DeltaTime;
					SoldierMovement.State = MoveDelta.IsNearlyZero()
						? EAlgonaSoldierMovementState::Idle
						: EAlgonaSoldierMovementState::Moving;
					SoldierMovement.DistanceState = EAlgonaSoldierDistanceState::Lost;

					if (!SoldierMovement.Velocity.IsNearlyZero())
					{
						Transform.SetRotation(RotateSoldierBodyToward(
							OldRotation,
							SoldierMovement.Velocity,
							SoldierMovement.BodyTurnSpeedScale,
							SoldierMovement.MotionVariationSeed,
							DeltaTime));
					}

					if (FVector::DistSquared(NewLocation, Squad.AnchorLocation)
						<= FMath::Square(
							static_cast<double>(AlgonaSimulationDefaults::LostRecoveryDistanceCm)))
					{
						const uint32 StableId = Ids[Index].Value;
						if (StableId > 0)
						{
							RecoveredLostSoldierIndices.Add(
								static_cast<int32>(StableId - 1));
						}
					}

					if (!OldLocation.Equals(NewLocation, 0.01)
						|| !OldRotation.Equals(Transform.GetRotation(), 1.0e-5f))
					{
						++ChangedEntities;
					}
					continue;
				}

				if (!Squad.Formation.IsValidSlot(Member.SlotIndex))
				{
					SoldierMovement.Velocity = FVector::ZeroVector;
					SoldierMovement.State = EAlgonaSoldierMovementState::Idle;
					SoldierMovement.DistanceState = EAlgonaSoldierDistanceState::Far;
					++Squad.DistanceCounters.Far;
					continue;
				}

				const FVector CanonicalSlotLocation = ComputeSlotWorldPosition(
					Squad,
					Member.SlotIndex);
				const FVector DesiredLocation = CanonicalSlotLocation
					+ ComputePersonalMovementOffset(
						Squad,
						SoldierMovement);
				const FVector ToDesiredLocation = DesiredLocation - OldLocation;
				const double PersonalSpeedScale = static_cast<double>(FMath::Clamp(
					SoldierMovement.PersonalSpeedScale,
					0.85f,
					1.0f));

				// Most individuality still comes from longitudinal follow speed. Lateral
				// drift changes only over 15-30 m of real travel and is SmoothStep-blended
				// between fixed targets, so it cannot become per-tick skating. Catch-up
				// still stays under the 1.25x cap.
				const double ActiveSpeedLimit =
					static_cast<double>(Squad.GetActiveSoldierSpeedLimit());
				double FollowSpeed = ActiveSpeedLimit;
				if (Squad.bHasMoveTarget
					&& !Squad.bCenterAtDestination
					&& !Squad.bWaitingForStragglers)
				{
					const double PersonalCruiseSpeed =
						static_cast<double>(Squad.GetCenterMoveSpeed())
						* PersonalSpeedScale;
					const double CatchupSpeed =
						ToDesiredLocation.Length() * FollowCatchupPerSecond;
					FollowSpeed = FMath::Min(
						ActiveSpeedLimit,
						PersonalCruiseSpeed + CatchupSpeed);
				}

				const double MaxMoveDistance =
					FollowSpeed * static_cast<double>(DeltaTime);
				const FVector MoveDelta =
					ToDesiredLocation.GetClampedToMaxSize(MaxMoveDistance);
				const FVector NewLocation = OldLocation + MoveDelta;

				Transform.SetLocation(NewLocation);
				SoldierMovement.Velocity = MoveDelta / DeltaTime;
				SoldierMovement.State = MoveDelta.IsNearlyZero()
					? EAlgonaSoldierMovementState::Idle
					: EAlgonaSoldierMovementState::Moving;

				// A moving soldier faces the direction he physically travelled this step,
				// rather than sliding sideways while the squad turns or reforms. Once he
				// reaches his current desired position, smoothly return his body toward the
				// squad facing. Mirror assignment may still flip the logical formation
				// instantly, but body rotation always remains rate-limited.
				FVector DesiredBodyFacing = Squad.FacingDirection.GetSafeNormal2D();
				if (!SoldierMovement.Velocity.IsNearlyZero())
				{
					DesiredBodyFacing = SoldierMovement.Velocity.GetSafeNormal2D();
				}
				if (DesiredBodyFacing.IsNearlyZero())
				{
					DesiredBodyFacing = FVector::ForwardVector;
				}
				Transform.SetRotation(RotateSoldierBodyToward(
					OldRotation,
					DesiredBodyFacing,
					SoldierMovement.BodyTurnSpeedScale,
					SoldierMovement.MotionVariationSeed,
					DeltaTime));

				if (Squad.bReformInProgress)
				{
					// Reform temporarily disables cohesion classification. Track only
					// physical settling into the new moving targets.
					if (FVector::DistSquared(NewLocation, DesiredLocation)
						> FMath::Square(ReformCompletionToleranceCm))
					{
						++Squad.ReformUnsettledActiveCount;
					}

					SoldierMovement.DistanceState = EAlgonaSoldierDistanceState::Near;
					++Squad.DistanceCounters.Near;
				}
				else
				{
					// Outside distance is measured in the squad's local X/Y relative
					// to the rectangle width/depth. A long 50x1 formation therefore
					// remains valid along its line but still detects perpendicular lag.
					const double DistanceOutsideFormationSquared =
						ComputeDistanceOutsideFormationSquared2D(Squad, NewLocation);
					const double NearDistanceSquared = FMath::Square(
						static_cast<double>(AlgonaSimulationDefaults::NearFormationMarginCm));
					const double MediumDistanceSquared = FMath::Square(
						static_cast<double>(AlgonaSimulationDefaults::MediumFormationMarginCm));

					if (DistanceOutsideFormationSquared <= NearDistanceSquared)
					{
						SoldierMovement.DistanceState = EAlgonaSoldierDistanceState::Near;
						++Squad.DistanceCounters.Near;
					}
					else if (DistanceOutsideFormationSquared <= MediumDistanceSquared)
					{
						SoldierMovement.DistanceState = EAlgonaSoldierDistanceState::Medium;
						++Squad.DistanceCounters.Medium;
					}
					else
					{
						SoldierMovement.DistanceState = EAlgonaSoldierDistanceState::Far;
						++Squad.DistanceCounters.Far;
					}
				}

				if (!OldLocation.Equals(NewLocation, 0.01)
					|| !OldRotation.Equals(Transform.GetRotation(), 1.0e-5f))
				{
					++ChangedEntities;
				}
			}
		});

	// Reform is considered physically complete only after every active member
	// reaches its current desired position. The 1 cm value is only a numerical
	// completion tolerance, not a gameplay Near/Medium/Far threshold.
	for (FAlgonaSquad& Squad : Squads)
	{
		if (Squad.bReformInProgress
			&& Squad.ReformUnsettledActiveCount == 0)
		{
			Squad.bReformInProgress = false;
		}
	}

	// Recovery is a rare structural event and reforms only affected squads.
	ProcessRecoveredLostMembers(RecoveredLostSoldierIndices);
	return ChangedEntities;
}

void UAlgonaSimulationSubsystem::EvaluateSquadCohesion(float DeltaTime)
{
	for (FAlgonaSquad& Squad : Squads)
	{
		const int32 ActiveCount = Squad.ActiveMemberSoldierIndices.Num();

		if (Squad.bCenterAtDestination)
		{
			// Final facing is part of the held-RMB order. Do not consume the five-
			// second assembly timeout until the logical formation turn is complete.
			if (Squad.bFinalFacingPending)
			{
				continue;
			}

			// Final-position obstacle handling is deliberately separate from normal
			// cohesion. A blocked final slot must neither move the exact center nor
			// turn its soldier Lost. The timeout prevents an endless command.
			const bool bFormationAssembled =
				ActiveCount <= 0
				|| (!Squad.bReformInProgress
					&& Squad.DistanceCounters.Near >= ActiveCount);

			if (bFormationAssembled)
			{
				FinishSquadMoveCommand(Squad);
				continue;
			}

			Squad.FinalAssemblyRemainingSeconds = FMath::Max(
				0.0f,
				Squad.FinalAssemblyRemainingSeconds - DeltaTime);
			if (Squad.FinalAssemblyRemainingSeconds <= 0.0f)
			{
				FinishSquadMoveCommand(Squad);
			}
			continue;
		}

		if (!Squad.bHasMoveTarget || ActiveCount <= 0)
		{
			Squad.CohesionMoveScale = 1.0f;
			continue;
		}

		if (Squad.bReformInProgress)
		{
			Squad.bWaitingForStragglers = false;
			Squad.StragglerWaitRemainingSeconds = 0.0f;
			Squad.CohesionMoveScale = 1.0f;
			continue;
		}

		const int32 FarCount = Squad.DistanceCounters.Far;
		const int32 MediumAndFarCount =
			Squad.DistanceCounters.Medium + FarCount;

		if (Squad.bWaitingForStragglers)
		{
			// Resume immediately once no active soldier remains Far.
			if (FarCount == 0)
			{
				Squad.bWaitingForStragglers = false;
				Squad.StragglerWaitRemainingSeconds = 0.0f;
				Squad.CohesionMoveScale =
					MediumAndFarCount * 5 >= ActiveCount ? 0.5f : 1.0f;
				continue;
			}

			Squad.StragglerWaitRemainingSeconds = FMath::Max(
				0.0f,
				Squad.StragglerWaitRemainingSeconds - DeltaTime);

			if (Squad.StragglerWaitRemainingSeconds > 0.0f)
			{
				Squad.CohesionMoveScale = 0.0f;
				continue;
			}

			// Five seconds elapsed: only soldiers still Far at this exact moment
			// become Lost. Reform is O(this squad), not another Mass pass.
			MarkCurrentFarMembersLost(Squad);
			Squad.bWaitingForStragglers = false;
			Squad.StragglerWaitRemainingSeconds = 0.0f;
			Squad.CohesionMoveScale = 1.0f;
			continue;
		}

		// Integer multiplication keeps the exact >=20% rule without float noise.
		if (FarCount * 5 >= ActiveCount)
		{
			Squad.bWaitingForStragglers = true;
			Squad.StragglerWaitRemainingSeconds =
				AlgonaSimulationDefaults::StragglerWaitSeconds;
			Squad.CohesionMoveScale = 0.0f;
		}
		else if (MediumAndFarCount * 5 >= ActiveCount)
		{
			Squad.CohesionMoveScale = 0.5f;
		}
		else
		{
			Squad.CohesionMoveScale = 1.0f;
		}
	}
}
