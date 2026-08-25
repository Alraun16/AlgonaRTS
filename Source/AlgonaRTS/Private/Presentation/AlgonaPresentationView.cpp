#include "AlgonaPresentationView.h"

#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

bool FAlgonaPresentationView::Build(
	UWorld& World,
	const UCameraComponent& Camera)
{
	bValid = false;
	GroundTopLeft = FVector::ZeroVector;
	ViewRight = FVector::ZeroVector;
	ViewDown = FVector::ZeroVector;
	ViewDeterminant = 0.0;
	GroundPixelsPerWorldUnit = 0.0f;
	VerticalProjectionFactor = 0.0f;
	ViewportWidth = 0;
	ViewportHeight = 0;

	if (Camera.ProjectionMode != ECameraProjectionMode::Orthographic
		|| Camera.OrthoWidth <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	APlayerController* PlayerController = World.GetFirstPlayerController();
	if (!PlayerController)
	{
		return false;
	}

	PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
	if (ViewportWidth <= 0 || ViewportHeight <= 0)
	{
		return false;
	}

	const FVector CameraForward = Camera.GetForwardVector();
	if (FMath::Abs(CameraForward.Z) <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const double ViewAspectRatio =
		static_cast<double>(ViewportWidth)
		/ static_cast<double>(ViewportHeight);

	const double HalfViewWidth =
		static_cast<double>(Camera.OrthoWidth) * 0.5;
	const double HalfViewHeight = HalfViewWidth / ViewAspectRatio;

	const FVector CameraLocation = Camera.GetComponentLocation();
	const FVector CameraRight = Camera.GetRightVector();
	const FVector CameraUp = Camera.GetUpVector();

	auto ProjectCornerToGround =
		[&](double RightOffset, double UpOffset, FVector& OutGroundPosition)
		{
			const FVector RayOrigin =
				CameraLocation
				+ CameraRight * RightOffset
				+ CameraUp * UpOffset;

			const double Distance = -RayOrigin.Z / CameraForward.Z;
			if (Distance < 0.0)
			{
				return false;
			}

			OutGroundPosition = RayOrigin + CameraForward * Distance;
			OutGroundPosition.Z = 0.0;
			return true;
		};

	FVector GroundTopRight = FVector::ZeroVector;
	FVector GroundBottomLeft = FVector::ZeroVector;

	if (!ProjectCornerToGround(-HalfViewWidth, HalfViewHeight, GroundTopLeft)
		|| !ProjectCornerToGround(HalfViewWidth, HalfViewHeight, GroundTopRight)
		|| !ProjectCornerToGround(-HalfViewWidth, -HalfViewHeight, GroundBottomLeft))
	{
		return false;
	}

	ViewRight = GroundTopRight - GroundTopLeft;
	ViewDown = GroundBottomLeft - GroundTopLeft;
	ViewDeterminant =
		ViewRight.X * ViewDown.Y - ViewRight.Y * ViewDown.X;

	if (FMath::Abs(ViewDeterminant) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}

	GroundPixelsPerWorldUnit =
		static_cast<float>(ViewportWidth) / Camera.OrthoWidth;

	VerticalProjectionFactor = static_cast<float>(
		FMath::Abs(FVector::DotProduct(FVector::UpVector, CameraUp)));

	bValid = true;
	return true;
}

bool FAlgonaPresentationView::IsGroundPointVisible(
	const FVector& WorldPosition,
	double GuardPixels) const
{
	// Fail open: visibility must never accidentally remove the whole army
	// when the camera footprint cannot be built.
	if (!bValid)
	{
		return true;
	}

	const FVector ToPoint = WorldPosition - GroundTopLeft;

	const double Horizontal =
		(ToPoint.X * ViewDown.Y - ToPoint.Y * ViewDown.X)
		/ ViewDeterminant;

	const double Vertical =
		(ViewRight.X * ToPoint.Y - ViewRight.Y * ToPoint.X)
		/ ViewDeterminant;

	const double HorizontalGuard =
		ViewportWidth > 0
			? GuardPixels / static_cast<double>(ViewportWidth)
			: 0.0;

	const double VerticalGuard =
		ViewportHeight > 0
			? GuardPixels / static_cast<double>(ViewportHeight)
			: 0.0;

	return Horizontal >= -HorizontalGuard
		&& Horizontal <= 1.0 + HorizontalGuard
		&& Vertical >= -VerticalGuard
		&& Vertical <= 1.0 + VerticalGuard;
}

float FAlgonaPresentationView::GetProjectedVerticalSizePixels(
	double WorldHeight) const
{
	if (!bValid || WorldHeight <= 0.0)
	{
		return 0.0f;
	}

	return static_cast<float>(
		WorldHeight
		* static_cast<double>(VerticalProjectionFactor)
		* static_cast<double>(GroundPixelsPerWorldUnit));
}
