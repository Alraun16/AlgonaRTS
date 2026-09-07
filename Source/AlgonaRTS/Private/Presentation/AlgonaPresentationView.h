#pragma once

#include "CoreMinimal.h"

class UCameraComponent;
class UWorld;

/**
 * Renderer-neutral description of the local camera footprint on the ground.
 * Orthographic keeps the proven ground-plane math. Perspective caches the
 * current UE view-projection data once, then uses only local matrix math for
 * per-unit visibility checks.
 */
struct FAlgonaPresentationView
{
	bool Build(UWorld& World, const UCameraComponent& Camera);

	bool IsGroundPointVisible(
		const FVector& WorldPosition,
		double GuardPixels) const;

	/** Axis-aligned preliminary bounds of the guarded ground footprint. */
	bool GetGroundBounds(
		double GuardPixels,
		FVector2D& OutWorldMin,
		FVector2D& OutWorldMax) const;

	float GetProjectedVerticalSizePixels(double WorldHeight) const;

	bool IsValid() const { return bValid; }
	bool IsPerspectiveProjection() const { return bPerspectiveProjection; }
	const FIntRect& GetProjectionViewRect() const { return ProjectionViewRect; }
	const FMatrix& GetViewProjectionMatrix() const { return ViewProjectionMatrix; }
	float GetGroundPixelsPerWorldUnit() const { return GroundPixelsPerWorldUnit; }
	int32 GetViewportWidth() const { return ViewportWidth; }
	int32 GetViewportHeight() const { return ViewportHeight; }

private:
	bool DeprojectPerspectiveScreenPointToGround(
		double ScreenX,
		double ScreenY,
		FVector& OutGroundPosition) const;

	FVector GroundTopLeft = FVector::ZeroVector;
	FVector ViewRight = FVector::ZeroVector;
	FVector ViewDown = FVector::ZeroVector;

	FIntRect ProjectionViewRect = FIntRect(0, 0, 0, 0);
	FMatrix ViewProjectionMatrix = FMatrix::Identity;
	FMatrix InverseViewProjectionMatrix = FMatrix::Identity;

	double ViewDeterminant = 0.0;
	float GroundPixelsPerWorldUnit = 0.0f;
	float VerticalProjectionFactor = 0.0f;
	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	bool bPerspectiveProjection = false;
	bool bValid = false;
};
