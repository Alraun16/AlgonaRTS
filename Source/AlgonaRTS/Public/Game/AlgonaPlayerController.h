#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "AlgonaPlayerController.generated.h"

class AAlgonaRTSCameraActor;
class AAlgonaSelectionPresentationActor;
struct FAlgonaSquad;

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

	/** Рамка выбора в пикселях viewport, если игрок сейчас её тянет. */
	bool GetSelectionBox(FVector2D& OutMin, FVector2D& OutMax) const;

private:
	void UpdateCameraInput(float DeltaTime);
	void UpdateSelectionInput();

	// Squad, Unit которого находится под курсором, или INDEX_NONE.
	int32 PickSquadUnderCursor() const;

	// Squad, хотя бы один Unit которых попадает в экранную рамку.
	void CollectSquadsInScreenBox(
		const FVector2D& BoxMin,
		const FVector2D& BoxMax,
		TArray<int32>& OutSquadIds) const;

	void UpdateSelectionRings();

	UPROPERTY(Transient)
	TObjectPtr<AAlgonaRTSCameraActor> CameraActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AAlgonaSelectionPresentationActor> SelectionPresentation = nullptr;

	float CameraRotationSensitivity = 2.0f;

	TArray<int32> SelectedSquadIds;

	// Состояние ЛКМ: точка нажатия, текущая точка и признак рамки.
	FVector2D LeftMousePressPosition = FVector2D::ZeroVector;
	FVector2D LeftMouseCurrentPosition = FVector2D::ZeroVector;
	bool bLeftMousePressed = false;
	bool bBoxSelecting = false;

	// Переиспользуемый буфер transform кругов, чтобы не выделять память каждый кадр.
	TArray<FTransform> RingTransforms;
};
