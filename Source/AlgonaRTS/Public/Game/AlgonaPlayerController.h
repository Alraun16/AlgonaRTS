#pragma once

// Minimal gameplay input owner; RTS camera remains a separate actor.
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "AlgonaPlayerController.generated.h"

/**
 * P2 single-squad command controller.
 * LMB selects the nearest squad center. A quick RMB issues a normal move;
 * held RMB draws a final front line whose length sets rectangle width and whose
 * drag direction determines the final facing. Multi-selection remains later.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaPlayerController final : public APlayerController
{
	GENERATED_BODY()

public:
	AAlgonaPlayerController();

	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;

private:
	bool TryGetGroundPointFromRay(
		const FVector& RayOrigin,
		const FVector& RayDirection,
		FVector& OutGroundPoint) const;
	bool TryGetCursorGroundPoint(FVector& OutGroundPoint) const;
	bool TryGetScreenGroundPoint(
		const FVector2D& ScreenPosition,
		FVector& OutGroundPoint) const;
	bool TryBuildRightMouseFormation(
		FVector& OutTargetLocation,
		FVector& OutFinalFacing,
		double& OutRowLengthCm) const;
	void SelectSquadAtCursor();

	void BeginRightMouseCommand();
	void UpdateRightMouseCommandPreview();
	void FinishRightMouseCommand();
	void CancelRightMouseCommand();
	void DrawRightMouseCommandPreview() const;

	void DrawSelectedSquadMarker() const;

	int32 SelectedSquadId = INDEX_NONE;
	float SelectionSearchRadiusCm = 800.0f;
	float SelectionMarkerRadiusCm = 150.0f;

	// The drag line is the desired final front rank. Facing is first computed as
	// the perpendicular in screen space, then deprojected to the ground, so the
	// result follows what the player actually sees from the RTS camera.
	bool bRightMouseCommandActive = false;
	bool bRightMouseHasScreenPosition = false;
	FVector RightMousePressPoint = FVector::ZeroVector;
	FVector RightMouseCurrentPoint = FVector::ZeroVector;
	FVector2D RightMousePressScreen = FVector2D::ZeroVector;
	FVector2D RightMouseCurrentScreen = FVector2D::ZeroVector;
	float FormationDragThresholdPixels = 15.0f;
	float FacingPreviewLengthCm = 400.0f;
	float FacingPreviewArrowHeadCm = 100.0f;
	float FacingProjectionSamplePixels = 100.0f;
};
