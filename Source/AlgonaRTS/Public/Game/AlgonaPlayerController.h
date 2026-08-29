#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "AlgonaPlayerController.generated.h"

class AAlgonaRTSCameraActor;

/**
 * Owns local player input for RTS controls.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaPlayerController final : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void PlayerTick(float DeltaTime) override;

	void SetRTSCamera(AAlgonaRTSCameraActor* InCamera);

private:
	UPROPERTY(Transient)
	TObjectPtr<AAlgonaRTSCameraActor> CameraActor = nullptr;

	float CameraRotationSensitivity = 2.0f;
};