#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "AlgonaPlayerController.generated.h"

class AAlgonaRTSCameraActor;

/**
 * Owns local player input for RTS controls.
 * Выбор Squad — локальное состояние игрока: Simulation о нём не знает,
 * приказы в Simulation будут идти через очередь команд.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaPlayerController final : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;

	void SetRTSCamera(AAlgonaRTSCameraActor* InCamera);

private:
	void UpdateCameraInput(float DeltaTime);
	void UpdateSelectionInput();

	// Squad, Unit которого находится под курсором, или INDEX_NONE.
	int32 PickSquadUnderCursor() const;

	void DrawSelectedSquads() const;

	UPROPERTY(Transient)
	TObjectPtr<AAlgonaRTSCameraActor> CameraActor = nullptr;

	float CameraRotationSensitivity = 2.0f;

	TArray<int32> SelectedSquadIds;

	FVector2D LeftMousePressPosition = FVector2D::ZeroVector;
	bool bLeftMousePressed = false;
};
