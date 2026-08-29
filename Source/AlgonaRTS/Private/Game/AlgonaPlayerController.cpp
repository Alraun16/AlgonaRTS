#include "Game/AlgonaPlayerController.h"

// Renderer-neutral Simulation queries/commands; controller knows no Mass internals.
#include "Core/AlgonaSimulationSubsystem.h"
#include "Spatial/AlgonaSquadSpatialSnapshot.h"

// Cursor deprojection, world trace, input keys and lightweight command debug.
#include "CollisionQueryParams.h"
#include "DrawDebugHelpers.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

AAlgonaPlayerController::AAlgonaPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
	bShowMouseCursor = true;
}

void AAlgonaPlayerController::BeginPlay()
{
	Super::BeginPlay();
	bShowMouseCursor = true;
}

void AAlgonaPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	(void)DeltaTime;

	// Raw key polling remains enough for this P2 command prototype and avoids
	// introducing an input framework layer before multi-selection/UI is needed.
	if (WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		SelectSquadAtCursor();
	}

	if (WasInputKeyJustPressed(EKeys::RightMouseButton))
	{
		BeginRightMouseCommand();
	}

	if (bRightMouseCommandActive)
	{
		UpdateRightMouseCommandPreview();
	}

	if (WasInputKeyJustReleased(EKeys::RightMouseButton))
	{
		FinishRightMouseCommand();
	}

	DrawRightMouseCommandPreview();
	DrawSelectedSquadMarker();
}

bool AAlgonaPlayerController::TryGetGroundPointFromRay(
	const FVector& RayOrigin,
	const FVector& RayDirection,
	FVector& OutGroundPoint) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FVector SafeDirection = RayDirection.GetSafeNormal();
	if (SafeDirection.IsNearlyZero())
	{
		return false;
	}

	const FVector TraceEnd = RayOrigin + SafeDirection * 1000000.0;

	// Prefer actual world geometry so uneven terrain/navigation tests can use the
	// same controller. The flat Z=0 plane remains a fail-open P0/P1 test fallback.
	FHitResult Hit;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = true;
	if (World->LineTraceSingleByChannel(
		Hit,
		RayOrigin,
		TraceEnd,
		ECC_Visibility,
		QueryParams))
	{
		OutGroundPoint = Hit.ImpactPoint;
		return true;
	}

	if (FMath::IsNearlyZero(SafeDirection.Z))
	{
		return false;
	}

	const double DistanceToGroundPlane = -RayOrigin.Z / SafeDirection.Z;
	if (DistanceToGroundPlane < 0.0)
	{
		return false;
	}

	OutGroundPoint = RayOrigin + SafeDirection * DistanceToGroundPlane;
	return true;
}

bool AAlgonaPlayerController::TryGetCursorGroundPoint(
	FVector& OutGroundPoint) const
{
	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ZeroVector;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return false;
	}

	return TryGetGroundPointFromRay(
		RayOrigin,
		RayDirection,
		OutGroundPoint);
}

bool AAlgonaPlayerController::TryGetScreenGroundPoint(
	const FVector2D& ScreenPosition,
	FVector& OutGroundPoint) const
{
	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ZeroVector;
	if (!DeprojectScreenPositionToWorld(
		static_cast<float>(ScreenPosition.X),
		static_cast<float>(ScreenPosition.Y),
		RayOrigin,
		RayDirection))
	{
		return false;
	}

	return TryGetGroundPointFromRay(
		RayOrigin,
		RayDirection,
		OutGroundPoint);
}

bool AAlgonaPlayerController::TryBuildRightMouseFormation(
	FVector& OutTargetLocation,
	FVector& OutFinalFacing,
	double& OutRowLengthCm) const
{
	OutTargetLocation = FVector::ZeroVector;
	OutFinalFacing = FVector::ZeroVector;
	OutRowLengthCm = 0.0;

	if (!bRightMouseHasScreenPosition)
	{
		return false;
	}

	const FVector2D ScreenDrag =
		RightMouseCurrentScreen - RightMousePressScreen;
	if (ScreenDrag.IsNearlyZero())
	{
		return false;
	}

	FVector WorldDrag = RightMouseCurrentPoint - RightMousePressPoint;
	WorldDrag.Z = 0.0;
	OutRowLengthCm = WorldDrag.Length();
	OutTargetLocation =
		(RightMousePressPoint + RightMouseCurrentPoint) * 0.5;

	// Determine facing from the player's 2D gesture, not by taking a perpendicular
	// in world XY. With an oblique RTS camera those are visibly different.
	const FVector2D ScreenFacing = FVector2D(
		ScreenDrag.Y,
		-ScreenDrag.X).GetSafeNormal();
	const FVector2D ScreenMidpoint =
		(RightMousePressScreen + RightMouseCurrentScreen) * 0.5;
	const FVector2D FacingSampleScreen =
		ScreenMidpoint + ScreenFacing * FacingProjectionSamplePixels;

	FVector FacingOriginWorld = FVector::ZeroVector;
	FVector FacingSampleWorld = FVector::ZeroVector;
	if (TryGetScreenGroundPoint(ScreenMidpoint, FacingOriginWorld)
		&& TryGetScreenGroundPoint(FacingSampleScreen, FacingSampleWorld))
	{
		OutFinalFacing =
			(FacingSampleWorld - FacingOriginWorld).GetSafeNormal2D();
	}

	// Fail-open fallback keeps commands usable if deprojection unexpectedly fails.
	if (OutFinalFacing.IsNearlyZero())
	{
		const FVector RowDirection = WorldDrag.GetSafeNormal2D();
		OutFinalFacing = FVector(
			RowDirection.Y,
			-RowDirection.X,
			0.0);
	}

	return !OutFinalFacing.IsNearlyZero();
}

void AAlgonaPlayerController::SelectSquadAtCursor()
{
	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;
	if (!Simulation)
	{
		SelectedSquadId = INDEX_NONE;
		CancelRightMouseCommand();
		return;
	}

	FVector GroundPoint = FVector::ZeroVector;
	if (!TryGetCursorGroundPoint(GroundPoint))
	{
		SelectedSquadId = INDEX_NONE;
		CancelRightMouseCommand();
		return;
	}

	const FVector2D QueryCenter(GroundPoint.X, GroundPoint.Y);
	const FVector2D QueryExtent(
		SelectionSearchRadiusCm,
		SelectionSearchRadiusCm);

	TArray<FAlgonaSquadSpatialSnapshot> CandidateSquads;
	Simulation->QuerySquadsInBounds(
		QueryCenter - QueryExtent,
		QueryCenter + QueryExtent,
		CandidateSquads);

	SelectedSquadId = INDEX_NONE;
	double BestDistanceSquared =
		static_cast<double>(SelectionSearchRadiusCm)
		* static_cast<double>(SelectionSearchRadiusCm);

	// Selection works at squad granularity, matching future RTS multi-selection.
	for (const FAlgonaSquadSpatialSnapshot& Candidate : CandidateSquads)
	{
		const FVector2D CandidateCenter(Candidate.Center.X, Candidate.Center.Y);
		const double DistanceSquared =
			(QueryCenter - CandidateCenter).SizeSquared();
		if (DistanceSquared <= BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			SelectedSquadId = Candidate.SquadId;
		}
	}
}

void AAlgonaPlayerController::BeginRightMouseCommand()
{
	if (SelectedSquadId == INDEX_NONE)
	{
		CancelRightMouseCommand();
		return;
	}

	FVector GroundPoint = FVector::ZeroVector;
	if (!TryGetCursorGroundPoint(GroundPoint))
	{
		CancelRightMouseCommand();
		return;
	}

	bRightMouseCommandActive = true;
	RightMousePressPoint = GroundPoint;
	RightMouseCurrentPoint = GroundPoint;

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	bRightMouseHasScreenPosition = GetMousePosition(MouseX, MouseY);
	if (bRightMouseHasScreenPosition)
	{
		RightMousePressScreen = FVector2D(MouseX, MouseY);
		RightMouseCurrentScreen = RightMousePressScreen;
	}
}

void AAlgonaPlayerController::UpdateRightMouseCommandPreview()
{
	if (!bRightMouseCommandActive)
	{
		return;
	}

	FVector GroundPoint = FVector::ZeroVector;
	if (TryGetCursorGroundPoint(GroundPoint))
	{
		RightMouseCurrentPoint = GroundPoint;
	}

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (GetMousePosition(MouseX, MouseY))
	{
		if (!bRightMouseHasScreenPosition)
		{
			RightMousePressScreen = FVector2D(MouseX, MouseY);
			bRightMouseHasScreenPosition = true;
		}
		RightMouseCurrentScreen = FVector2D(MouseX, MouseY);
	}
}

void AAlgonaPlayerController::FinishRightMouseCommand()
{
	if (!bRightMouseCommandActive || SelectedSquadId == INDEX_NONE)
	{
		CancelRightMouseCommand();
		return;
	}

	UpdateRightMouseCommandPreview();

	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;
	if (!Simulation)
	{
		CancelRightMouseCommand();
		return;
	}

	const double ScreenDragPixels = bRightMouseHasScreenPosition
		? (RightMouseCurrentScreen - RightMousePressScreen).Size()
		: 0.0;

	// A small screen-space drag remains the original move-to-point command. Using
	// pixels here avoids making click tolerance depend on RTS camera zoom.
	if (ScreenDragPixels < FormationDragThresholdPixels)
	{
		Simulation->SubmitMoveSquadCommand(
			SelectedSquadId,
			RightMousePressPoint,
			EAlgonaSquadMovePace::Run);
		CancelRightMouseCommand();
		return;
	}

	FAlgonaSquadCommandPreview SquadPreview;
	if (!Simulation->GetSquadCommandPreview(SelectedSquadId, SquadPreview)
		|| SquadPreview.ActiveUnitCount <= 0
		|| SquadPreview.SlotSpacingCm <= KINDA_SMALL_NUMBER)
	{
		CancelRightMouseCommand();
		return;
	}

	FVector TargetLocation = FVector::ZeroVector;
	FVector FinalFacing = FVector::ZeroVector;
	double DragLengthCm = 0.0;
	if (!TryBuildRightMouseFormation(
		TargetLocation,
		FinalFacing,
		DragLengthCm))
	{
		CancelRightMouseCommand();
		return;
	}

	const int32 RequestedRowLength = FMath::Clamp(
		FMath::RoundToInt(
			static_cast<float>(DragLengthCm)
			/ SquadPreview.SlotSpacingCm) + 1,
		1,
		SquadPreview.ActiveUnitCount);

	// SubmitMoveSquadCommand itself rejects non-authoritative Simulation worlds.
	// Full remote-client command replication belongs to the networking stage.
	Simulation->SubmitMoveSquadCommand(
		SelectedSquadId,
		TargetLocation,
		EAlgonaSquadMovePace::Run,
		FinalFacing,
		RequestedRowLength);

	CancelRightMouseCommand();
}

void AAlgonaPlayerController::CancelRightMouseCommand()
{
	bRightMouseCommandActive = false;
	bRightMouseHasScreenPosition = false;
	RightMousePressPoint = FVector::ZeroVector;
	RightMouseCurrentPoint = FVector::ZeroVector;
	RightMousePressScreen = FVector2D::ZeroVector;
	RightMouseCurrentScreen = FVector2D::ZeroVector;
}

void AAlgonaPlayerController::DrawRightMouseCommandPreview() const
{
	if (!bRightMouseCommandActive || SelectedSquadId == INDEX_NONE)
	{
		return;
	}

	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;
	if (!Simulation)
	{
		return;
	}

	FAlgonaSquadCommandPreview SquadPreview;
	if (!Simulation->GetSquadCommandPreview(SelectedSquadId, SquadPreview)
		|| SquadPreview.ActiveUnitCount <= 0
		|| SquadPreview.SlotSpacingCm <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const double ScreenDragPixels = bRightMouseHasScreenPosition
		? (RightMouseCurrentScreen - RightMousePressScreen).Size()
		: 0.0;
	if (ScreenDragPixels < FormationDragThresholdPixels)
	{
		return;
	}

	FVector TargetLocation = FVector::ZeroVector;
	FVector FinalFacing = FVector::ZeroVector;
	double DragLengthCm = 0.0;
	if (!TryBuildRightMouseFormation(
		TargetLocation,
		FinalFacing,
		DragLengthCm))
	{
		return;
	}

	const FVector PreviewLift(0.0, 0.0, 20.0);
	const FVector ArrowStart = TargetLocation + PreviewLift;
	const FVector ArrowTip =
		ArrowStart + FinalFacing * FacingPreviewLengthCm;
	const FVector ArrowRight = FVector::CrossProduct(
		FVector::UpVector,
		FinalFacing).GetSafeNormal2D();

	// The line is the requested front rank. The arrow uses the exact same world
	// facing that will be submitted, after screen-space perpendicular projection.
	DrawDebugLine(
		World,
		RightMousePressPoint + PreviewLift,
		RightMouseCurrentPoint + PreviewLift,
		FColor::Yellow,
		false,
		0.0f,
		0,
		8.0f);
	DrawDebugLine(
		World,
		ArrowStart,
		ArrowTip,
		FColor::Yellow,
		false,
		0.0f,
		0,
		8.0f);
	DrawDebugLine(
		World,
		ArrowTip,
		ArrowTip
			- FinalFacing * FacingPreviewArrowHeadCm
			+ ArrowRight * FacingPreviewArrowHeadCm * 0.6f,
		FColor::Yellow,
		false,
		0.0f,
		0,
		8.0f);
	DrawDebugLine(
		World,
		ArrowTip,
		ArrowTip
			- FinalFacing * FacingPreviewArrowHeadCm
			- ArrowRight * FacingPreviewArrowHeadCm * 0.6f,
		FColor::Yellow,
		false,
		0.0f,
		0,
		8.0f);
}

void AAlgonaPlayerController::DrawSelectedSquadMarker() const
{
	if (SelectedSquadId == INDEX_NONE)
	{
		return;
	}

	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;
	if (!Simulation)
	{
		return;
	}

	FVector SquadCenter = FVector::ZeroVector;
	if (!Simulation->GetSquadCenter(SelectedSquadId, SquadCenter))
	{
		return;
	}

	DrawDebugSphere(
		World,
		SquadCenter + FVector(0.0, 0.0, 100.0),
		SelectionMarkerRadiusCm,
		16,
		FColor::Yellow,
		false,
		0.0f,
		0,
		8.0f);
}
