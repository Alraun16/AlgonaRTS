#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaP1TestCameraActor.generated.h"

class APlayerController;
class UCameraComponent;

/**
 * Временная ортографическая RTS-камера
 * для P1 Presentation experiment.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaP1TestCameraActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaP1TestCameraActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UCameraComponent* GetCameraComponent() const
	{
		return CameraComponent;
	}

private:
	void UpdateAspectRatioFromViewport(
		APlayerController& PlayerController);

	void UpdateZoom(
		APlayerController& PlayerController);

	UPROPERTY(VisibleAnywhere, Category = "Algona|P1 Test Camera")
	TObjectPtr<UCameraComponent> CameraComponent = nullptr;

	// Точка земли, на которую камера смотрит при старте.
	FVector InitialGroundFocus =
		FVector(10000.0, 15000.0, 0.0);

	float CameraHeight = 12000.0f;

	// 1.0 = примерно одна ширина экрана в секунду.
	float MoveSpeedOrthoWidthMultiplier = 1.0f;

	float MinOrthoWidth = 3000.0f;
	float MaxOrthoWidth = 60000.0f;

	// < 1: колесо вверх приближает.
	float ZoomFactorPerWheelStep = 0.85f;
};