#include "Camera/AlgonaRTSCameraActor.h"

#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

AAlgonaRTSCameraActor::AAlgonaRTSCameraActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	SetRootComponent(CameraComponent);

	CameraComponent->SetProjectionMode(ECameraProjectionMode::Orthographic);
	CameraComponent->SetOrthoWidth(20000.0f);
	CameraComponent->SetConstraintAspectRatio(true);
}

void AAlgonaRTSCameraActor::BeginPlay()
{
	Super::BeginPlay();

	const FRotator ViewRotation(-45.0f, -45.0f, 0.0f);
	SetActorRotation(ViewRotation);

	const FVector Forward = ViewRotation.Vector();
	const double DistanceToGround =
		static_cast<double>(CameraHeight)
		/ FMath::Max(static_cast<double>(-Forward.Z), 0.001);

	SetActorLocation(InitialGroundFocus - Forward * DistanceToGround);

	UWorld* World = GetWorld();
	APlayerController* PlayerController =
		World ? World->GetFirstPlayerController() : nullptr;

	if (!PlayerController)
	{
		return;
	}

	UpdateAspectRatioFromViewport(*PlayerController);
	PlayerController->SetViewTarget(this);
}

void AAlgonaRTSCameraActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	APlayerController* PlayerController =
		World ? World->GetFirstPlayerController() : nullptr;

	if (!PlayerController)
	{
		return;
	}

	UpdateAspectRatioFromViewport(*PlayerController);
	UpdateZoom(*PlayerController);

	FVector Forward = GetActorForwardVector();
	Forward.Z = 0.0;
	Forward.Normalize();

	FVector Right = GetActorRightVector();
	Right.Z = 0.0;
	Right.Normalize();

	FVector MoveDirection = FVector::ZeroVector;

	if (PlayerController->IsInputKeyDown(EKeys::W))
	{
		MoveDirection += Forward;
	}
	if (PlayerController->IsInputKeyDown(EKeys::S))
	{
		MoveDirection -= Forward;
	}
	if (PlayerController->IsInputKeyDown(EKeys::D))
	{
		MoveDirection += Right;
	}
	if (PlayerController->IsInputKeyDown(EKeys::A))
	{
		MoveDirection -= Right;
	}

	if (!MoveDirection.IsNearlyZero())
	{
		MoveDirection.Normalize();

		const float CurrentMoveSpeed =
			CameraComponent
				? CameraComponent->OrthoWidth * MoveSpeedOrthoWidthMultiplier
				: 20000.0f;

		AddActorWorldOffset(
			MoveDirection * CurrentMoveSpeed * DeltaSeconds,
			false);
	}
}

void AAlgonaRTSCameraActor::UpdateAspectRatioFromViewport(
	APlayerController& PlayerController)
{
	if (!CameraComponent)
	{
		return;
	}

	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	PlayerController.GetViewportSize(ViewportWidth, ViewportHeight);

	if (ViewportWidth <= 0 || ViewportHeight <= 0)
	{
		return;
	}

	CameraComponent->AspectRatio =
		static_cast<float>(ViewportWidth)
		/ static_cast<float>(ViewportHeight);
}

void AAlgonaRTSCameraActor::UpdateZoom(APlayerController& PlayerController)
{
	if (!CameraComponent)
	{
		return;
	}

	const float WheelDelta =
		PlayerController.GetInputAnalogKeyState(EKeys::MouseWheelAxis);

	if (FMath::IsNearlyZero(WheelDelta))
	{
		return;
	}

	const float ZoomFactor =
		FMath::Pow(ZoomFactorPerWheelStep, WheelDelta);

	CameraComponent->SetOrthoWidth(
		FMath::Clamp(
			CameraComponent->OrthoWidth * ZoomFactor,
			MinOrthoWidth,
			MaxOrthoWidth));
}
