#pragma once

#include "CoreMinimal.h"

class UCameraComponent;
class UWorld;

/**
 * Renderer-neutral description of the current orthographic camera footprint.
 * Both legacy and current renderers use this exact visibility calculation.
 */
struct FAlgonaPresentationView
{
	bool Build(UWorld& World, const UCameraComponent& Camera);

	bool IsGroundPointVisible(
		const FVector& WorldPosition,
		double GuardPixels) const;

	float GetProjectedVerticalSizePixels(double WorldHeight) const;

	bool IsValid() const { return bValid; }
	float GetGroundPixelsPerWorldUnit() const { return GroundPixelsPerWorldUnit; }
	int32 GetViewportWidth() const { return ViewportWidth; }
	int32 GetViewportHeight() const { return ViewportHeight; }

private:
	FVector GroundTopLeft = FVector::ZeroVector;
	FVector ViewRight = FVector::ZeroVector;
	FVector ViewDown = FVector::ZeroVector;

	double ViewDeterminant = 0.0;
	float GroundPixelsPerWorldUnit = 0.0f;
	float VerticalProjectionFactor = 0.0f;
	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	bool bValid = false;
};
