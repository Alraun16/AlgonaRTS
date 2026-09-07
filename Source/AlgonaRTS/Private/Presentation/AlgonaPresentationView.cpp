#include "AlgonaPresentationView.h"

#include "Camera/CameraComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"

namespace
{
	// Algona currently uses a normal single non-stereo gameplay view.
	constexpr int32 PrimaryViewIndex = 0;
}

bool FAlgonaPresentationView::Build(
	UWorld& World,
	const UCameraComponent& Camera)
{
	bValid = false;
	bPerspectiveProjection = false;
	GroundTopLeft = FVector::ZeroVector;
	ViewRight = FVector::ZeroVector;
	ViewDown = FVector::ZeroVector;
	ProjectionViewRect = FIntRect(0, 0, 0, 0);
	ViewProjectionMatrix = FMatrix::Identity;
	InverseViewProjectionMatrix = FMatrix::Identity;
	ViewDeterminant = 0.0;
	GroundPixelsPerWorldUnit = 0.0f;
	VerticalProjectionFactor = 0.0f;
	ViewportWidth = 0;
	ViewportHeight = 0;

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

	if (Camera.ProjectionMode == ECameraProjectionMode::Perspective)
	{
		// GetProjectionData describes the player's real rendered view. Require
		// the requested Presentation camera to be that active view target.
		if (PlayerController->GetViewTarget() != Camera.GetOwner())
		{
			return false;
		}

		ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
		UGameViewportClient* ViewportClient =
			LocalPlayer ? LocalPlayer->ViewportClient : nullptr;
		if (!LocalPlayer || !ViewportClient || !ViewportClient->Viewport)
		{
			return false;
		}

		FSceneViewProjectionData ProjectionData;
		if (!LocalPlayer->GetProjectionData(
				ViewportClient->Viewport,
				ProjectionData,
				PrimaryViewIndex)
			|| !ProjectionData.IsPerspectiveProjection()
			|| !ProjectionData.IsValidViewRectangle())
		{
			return false;
		}

		ProjectionViewRect = ProjectionData.GetConstrainedViewRect();
		if (ProjectionViewRect.Width() <= 0 || ProjectionViewRect.Height() <= 0)
		{
			return false;
		}

		ViewProjectionMatrix = ProjectionData.ComputeViewProjectionMatrix();
		InverseViewProjectionMatrix = ViewProjectionMatrix.InverseFast();
		ViewportWidth = ProjectionViewRect.Width();
		ViewportHeight = ProjectionViewRect.Height();
		bPerspectiveProjection = true;

		// Confirm that all four corners of the current perspective view reach
		// the ground plane. GetGroundBounds will use the same cached projection
		// with the requested pixel guard.
		FVector GroundTopRight = FVector::ZeroVector;
		FVector GroundBottomLeft = FVector::ZeroVector;
		FVector GroundBottomRight = FVector::ZeroVector;

		if (!DeprojectPerspectiveScreenPointToGround(
				static_cast<double>(ProjectionViewRect.Min.X),
				static_cast<double>(ProjectionViewRect.Min.Y),
				GroundTopLeft)
			|| !DeprojectPerspectiveScreenPointToGround(
				static_cast<double>(ProjectionViewRect.Max.X),
				static_cast<double>(ProjectionViewRect.Min.Y),
				GroundTopRight)
			|| !DeprojectPerspectiveScreenPointToGround(
				static_cast<double>(ProjectionViewRect.Min.X),
				static_cast<double>(ProjectionViewRect.Max.Y),
				GroundBottomLeft)
			|| !DeprojectPerspectiveScreenPointToGround(
				static_cast<double>(ProjectionViewRect.Max.X),
				static_cast<double>(ProjectionViewRect.Max.Y),
				GroundBottomRight))
		{
			return false;
		}

		bValid = true;
		return true;
	}

	if (Camera.ProjectionMode != ECameraProjectionMode::Orthographic
		|| Camera.OrthoWidth <= KINDA_SMALL_NUMBER)
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

bool FAlgonaPresentationView::DeprojectPerspectiveScreenPointToGround(
	double ScreenX,
	double ScreenY,
	FVector& OutGroundPosition) const
{
	if (!bPerspectiveProjection
		|| ProjectionViewRect.Width() <= 0
		|| ProjectionViewRect.Height() <= 0)
	{
		return false;
	}

	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ZeroVector;
	FSceneView::DeprojectScreenToWorld(
		FVector2D(ScreenX, ScreenY),
		ProjectionViewRect,
		InverseViewProjectionMatrix,
		RayOrigin,
		RayDirection);

	if (FMath::Abs(RayDirection.Z) <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const double Distance = -RayOrigin.Z / RayDirection.Z;
	if (Distance < 0.0)
	{
		return false;
	}

	OutGroundPosition = RayOrigin + RayDirection * Distance;
	OutGroundPosition.Z = 0.0;
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

	if (bPerspectiveProjection)
	{
		// This is the hot path. Projection data was captured once in Build();
		// every candidate now costs only one matrix transform and a few scalars.
		const FVector4 ClipPosition =
			ViewProjectionMatrix.TransformFVector4(
				FVector4(WorldPosition, 1.0));

		if (ClipPosition.W <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		const double InverseW = 1.0 / ClipPosition.W;
		const double NdcX = ClipPosition.X * InverseW;
		const double NdcY = ClipPosition.Y * InverseW;

		const double HorizontalGuard =
			2.0 * FMath::Max(GuardPixels, 0.0)
			/ static_cast<double>(ProjectionViewRect.Width());
		const double VerticalGuard =
			2.0 * FMath::Max(GuardPixels, 0.0)
			/ static_cast<double>(ProjectionViewRect.Height());

		return NdcX >= -1.0 - HorizontalGuard
			&& NdcX <= 1.0 + HorizontalGuard
			&& NdcY >= -1.0 - VerticalGuard
			&& NdcY <= 1.0 + VerticalGuard;
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

bool FAlgonaPresentationView::GetGroundBounds(
	double GuardPixels,
	FVector2D& OutWorldMin,
	FVector2D& OutWorldMax) const
{
	if (!bValid || ViewportWidth <= 0 || ViewportHeight <= 0)
	{
		OutWorldMin = FVector2D::ZeroVector;
		OutWorldMax = FVector2D::ZeroVector;
		return false;
	}

	FVector TopLeft = FVector::ZeroVector;
	FVector TopRight = FVector::ZeroVector;
	FVector BottomLeft = FVector::ZeroVector;
	FVector BottomRight = FVector::ZeroVector;

	if (bPerspectiveProjection)
	{
		const double SafeGuardPixels = FMath::Max(GuardPixels, 0.0);
		const double MinX =
			static_cast<double>(ProjectionViewRect.Min.X) - SafeGuardPixels;
		const double MinY =
			static_cast<double>(ProjectionViewRect.Min.Y) - SafeGuardPixels;
		const double MaxX =
			static_cast<double>(ProjectionViewRect.Max.X) + SafeGuardPixels;
		const double MaxY =
			static_cast<double>(ProjectionViewRect.Max.Y) + SafeGuardPixels;

		if (!DeprojectPerspectiveScreenPointToGround(MinX, MinY, TopLeft)
			|| !DeprojectPerspectiveScreenPointToGround(MaxX, MinY, TopRight)
			|| !DeprojectPerspectiveScreenPointToGround(MinX, MaxY, BottomLeft)
			|| !DeprojectPerspectiveScreenPointToGround(MaxX, MaxY, BottomRight))
		{
			OutWorldMin = FVector2D::ZeroVector;
			OutWorldMax = FVector2D::ZeroVector;
			return false;
		}
	}
	else
	{
		const double HorizontalGuard =
			GuardPixels / static_cast<double>(ViewportWidth);
		const double VerticalGuard =
			GuardPixels / static_cast<double>(ViewportHeight);

		TopLeft =
			GroundTopLeft
			- ViewRight * HorizontalGuard
			- ViewDown * VerticalGuard;
		TopRight =
			GroundTopLeft
			+ ViewRight * (1.0 + HorizontalGuard)
			- ViewDown * VerticalGuard;
		BottomLeft =
			GroundTopLeft
			- ViewRight * HorizontalGuard
			+ ViewDown * (1.0 + VerticalGuard);
		BottomRight =
			GroundTopLeft
			+ ViewRight * (1.0 + HorizontalGuard)
			+ ViewDown * (1.0 + VerticalGuard);
	}

	OutWorldMin = FVector2D(
		FMath::Min(
			FMath::Min(TopLeft.X, TopRight.X),
			FMath::Min(BottomLeft.X, BottomRight.X)),
		FMath::Min(
			FMath::Min(TopLeft.Y, TopRight.Y),
			FMath::Min(BottomLeft.Y, BottomRight.Y)));

	OutWorldMax = FVector2D(
		FMath::Max(
			FMath::Max(TopLeft.X, TopRight.X),
			FMath::Max(BottomLeft.X, BottomRight.X)),
		FMath::Max(
			FMath::Max(TopLeft.Y, TopRight.Y),
			FMath::Max(BottomLeft.Y, BottomRight.Y)));

	return true;
}

float FAlgonaPresentationView::GetProjectedVerticalSizePixels(
	double WorldHeight) const
{
	if (!bValid || WorldHeight <= 0.0)
	{
		return 0.0f;
	}

	if (bPerspectiveProjection)
	{
		// This is a single view-wide scale estimate at screen center. It is used
		// only for conservative culling margin; exact per-unit visibility still
		// uses the cached perspective projection matrix.
		const double CenterX =
			0.5 * static_cast<double>(ProjectionViewRect.Min.X + ProjectionViewRect.Max.X);
		const double CenterY =
			0.5 * static_cast<double>(ProjectionViewRect.Min.Y + ProjectionViewRect.Max.Y);

		FVector GroundCenter = FVector::ZeroVector;
		if (!DeprojectPerspectiveScreenPointToGround(CenterX, CenterY, GroundCenter))
		{
			return 0.0f;
		}

		const FVector TopPoint = GroundCenter + FVector::UpVector * WorldHeight;
		const FVector4 GroundClip =
			ViewProjectionMatrix.TransformFVector4(FVector4(GroundCenter, 1.0));
		const FVector4 TopClip =
			ViewProjectionMatrix.TransformFVector4(FVector4(TopPoint, 1.0));

		if (GroundClip.W <= UE_DOUBLE_SMALL_NUMBER
			|| TopClip.W <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0f;
		}

		const double GroundNdcY = GroundClip.Y / GroundClip.W;
		const double TopNdcY = TopClip.Y / TopClip.W;

		return static_cast<float>(
			FMath::Abs(TopNdcY - GroundNdcY)
			* 0.5
			* static_cast<double>(ProjectionViewRect.Height()));
	}

	return static_cast<float>(
		WorldHeight
		* static_cast<double>(VerticalProjectionFactor)
		* static_cast<double>(GroundPixelsPerWorldUnit));
}
