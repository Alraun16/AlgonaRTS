#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaRTSCameraActor.generated.h"

class UCameraComponent;

/**
 * RTS gameplay camera.
 * Owns the perspective view and camera transform.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaRTSCameraActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaRTSCameraActor();

	void MoveGroundFocus(
	float ForwardInput,
	float RightInput,
	float DeltaSeconds);

	void RotateYaw(float YawDeltaDegrees);
	void Zoom(float ZoomSteps);
	
	virtual void BeginPlay() override;

	UCameraComponent* GetCameraComponent() const
	{
		return CameraComponent;
	}

private:
	void UpdateCameraTransform();
	
	UPROPERTY(VisibleAnywhere, Category = "Algona|RTS Camera")
	TObjectPtr<UCameraComponent> CameraComponent = nullptr;

	// Point on the ground that the camera looks at and moves across the map.
	FVector GroundFocus = FVector::ZeroVector;

	// Base perspective camera parameters.
	float CameraHeight = 8000.0f;
	float MinCameraHeight = 1000.0f;
	float MaxCameraHeight = 20000.0f;
	float ZoomFactorPerStep = 0.85f;
	
	float DefaultCameraPitch = -35.0f;
	float CloseCameraPitch = -25.0f;

	float PitchChangeStartHeight = 3500.0f;
	float PitchChangeEndHeight = 1000.0f;
	
	float CameraYaw = 45.0f;
	float CameraFOV = 28.0f;
};
