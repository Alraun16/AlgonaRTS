#include "Camera/AlgonaRTSCameraActor.h"

#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"

AAlgonaRTSCameraActor::AAlgonaRTSCameraActor()
{
	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	SetRootComponent(CameraComponent);

	// Perspective RTS view.
	CameraComponent->SetProjectionMode(ECameraProjectionMode::Perspective);
	CameraComponent->SetFieldOfView(CameraFOV);
	CameraComponent->SetConstraintAspectRatio(false);
}

void AAlgonaRTSCameraActor::UpdateCameraTransform()
{
	const float PitchAlpha = FMath::Clamp(
		(PitchChangeStartHeight - CameraHeight)
		/ (PitchChangeStartHeight - PitchChangeEndHeight),
		0.0f,
		1.0f);

	const float CurrentPitch = FMath::Lerp(
		DefaultCameraPitch,
		CloseCameraPitch,
		PitchAlpha);

	const FRotator ViewRotation(
		CurrentPitch,
		CameraYaw,
		0.0f);

	const FVector Forward = ViewRotation.Vector();
	const double DistanceToGround =
		CameraHeight / -Forward.Z;

	SetActorRotation(ViewRotation);
	SetActorLocation(
		GroundFocus - Forward * DistanceToGround);
}

void AAlgonaRTSCameraActor::MoveGroundFocus(
	float ForwardInput,
	float RightInput,
	float DeltaSeconds)
{
	FVector Forward = GetActorForwardVector();
	Forward.Z = 0.0;
	Forward.Normalize();

	FVector Right = GetActorRightVector();
	Right.Z = 0.0;
	Right.Normalize();

	FVector MoveDirection =
		Forward * ForwardInput
		+ Right * RightInput;

	MoveDirection.Normalize();

	GroundFocus +=
		MoveDirection * (CameraHeight * 0.5f) * DeltaSeconds;

	UpdateCameraTransform();
}

void AAlgonaRTSCameraActor::RotateYaw(
	float YawDeltaDegrees)
{
	CameraYaw += YawDeltaDegrees;

	UpdateCameraTransform();
}

void AAlgonaRTSCameraActor::Zoom(float ZoomSteps)
{
	CameraHeight = FMath::Clamp(
		CameraHeight
			* FMath::Pow(ZoomFactorPerStep, ZoomSteps),
		MinCameraHeight,
		MaxCameraHeight);

	UpdateCameraTransform();

#if !UE_BUILD_SHIPPING
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			1001,
			1.0f,
			FColor::White,
			FString::Printf(
				TEXT("Camera Height: %.0f"),
				CameraHeight));
	}
#endif
}

void AAlgonaRTSCameraActor::BeginPlay()
{
	Super::BeginPlay();

	UpdateCameraTransform();
}
