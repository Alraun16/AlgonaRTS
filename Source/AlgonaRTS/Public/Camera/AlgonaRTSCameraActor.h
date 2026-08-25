#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaRTSCameraActor.generated.h"

class APlayerController;
class UCameraComponent;

/**
 * Minimal working RTS camera for the current P1 gameplay loop.
 * Owns orthographic view, WASD movement and wheel zoom.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaRTSCameraActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaRTSCameraActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UCameraComponent* GetCameraComponent() const
	{
		return CameraComponent;
	}

private:
	void UpdateAspectRatioFromViewport(APlayerController& PlayerController);
	void UpdateZoom(APlayerController& PlayerController);

	UPROPERTY(VisibleAnywhere, Category = "Algona|RTS Camera")
	TObjectPtr<UCameraComponent> CameraComponent = nullptr;

	FVector InitialGroundFocus = FVector(10000.0, 15000.0, 0.0);
	float CameraHeight = 12000.0f;

	// 1.0 moves approximately one visible screen width per second.
	float MoveSpeedOrthoWidthMultiplier = 1.0f;

	float MinOrthoWidth = 3000.0f;
	float MaxOrthoWidth = 60000.0f;
	float ZoomFactorPerWheelStep = 0.85f;
};
