#include "Debug/AlgonaP1TestCameraActor.h"

#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

AAlgonaP1TestCameraActor::AAlgonaP1TestCameraActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	CameraComponent =
		CreateDefaultSubobject<UCameraComponent>(
			TEXT("Camera"));

	SetRootComponent(CameraComponent);

	CameraComponent->SetProjectionMode(
		ECameraProjectionMode::Orthographic);

	CameraComponent->SetOrthoWidth(20000.0f);

	/*
	 * AspectRatio будет выставляться под реальный viewport,
	 * чтобы renderer и наш Presentation culling
	 * использовали одну и ту же область.
	 */
	CameraComponent->SetConstraintAspectRatio(true);
}

void AAlgonaP1TestCameraActor::BeginPlay()
{
	Super::BeginPlay();

	/*
	 * Явная RTS/isometric-подобная ориентация:
	 * 45 градусов вниз и 45 градусов по горизонтали.
	 */
	const FRotator ViewRotation(
		-45.0f,
		-45.0f,
		0.0f);

	SetActorRotation(ViewRotation);

	/*
	 * Ставим камеру так, чтобы её центральный луч
	 * пересекал землю ровно в InitialGroundFocus.
	 */
	const FVector Forward =
		ViewRotation.Vector();

	const double DistanceToGround =
		static_cast<double>(CameraHeight)
		/ FMath::Max(
			static_cast<double>(-Forward.Z),
			0.001);

	SetActorLocation(
		InitialGroundFocus
		- Forward * DistanceToGround);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PlayerController =
		World->GetFirstPlayerController();

	if (!PlayerController)
	{
		return;
	}

	UpdateAspectRatioFromViewport(
		*PlayerController);

	PlayerController->SetViewTarget(this);
}

void AAlgonaP1TestCameraActor::Tick(
	float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PlayerController =
		World->GetFirstPlayerController();

	if (!PlayerController)
	{
		return;
	}

	UpdateAspectRatioFromViewport(
		*PlayerController);

	UpdateZoom(
		*PlayerController);

	FVector Forward =
		GetActorForwardVector();

	Forward.Z = 0.0;
	Forward.Normalize();

	FVector Right =
		GetActorRightVector();

	Right.Z = 0.0;
	Right.Normalize();

	FVector MoveDirection =
		FVector::ZeroVector;

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
				? CameraComponent->OrthoWidth
					* MoveSpeedOrthoWidthMultiplier
				: 20000.0f;

		AddActorWorldOffset(
			MoveDirection
			* CurrentMoveSpeed
			* DeltaSeconds,
			false);
	}
}

void AAlgonaP1TestCameraActor::UpdateAspectRatioFromViewport(
	APlayerController& PlayerController)
{
	if (!CameraComponent)
	{
		return;
	}

	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;

	PlayerController.GetViewportSize(
		ViewportWidth,
		ViewportHeight);

	if (ViewportWidth <= 0
		|| ViewportHeight <= 0)
	{
		return;
	}

	CameraComponent->AspectRatio =
		static_cast<float>(ViewportWidth)
		/ static_cast<float>(ViewportHeight);
}

void AAlgonaP1TestCameraActor::UpdateZoom(
	APlayerController& PlayerController)
{
	if (!CameraComponent)
	{
		return;
	}

	const float WheelDelta =
		PlayerController.GetInputAnalogKeyState(
			EKeys::MouseWheelAxis);

	if (FMath::IsNearlyZero(WheelDelta))
	{
		return;
	}

	const float ZoomFactor =
		FMath::Pow(
			ZoomFactorPerWheelStep,
			WheelDelta);

	const float NewOrthoWidth =
		FMath::Clamp(
			CameraComponent->OrthoWidth
				* ZoomFactor,
			MinOrthoWidth,
			MaxOrthoWidth);

	CameraComponent->SetOrthoWidth(
		NewOrthoWidth);
}