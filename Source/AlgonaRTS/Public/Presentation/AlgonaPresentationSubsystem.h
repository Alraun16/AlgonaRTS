#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaPresentationSubsystem.generated.h"

class AAlgonaArmyPresentationActor;
class AAlgonaLegacyIsmPresentationActor;
class AAlgonaRTSCameraActor;

/**
 * Owns the local client-side camera and army rendering path.
 * Dedicated servers never create this subsystem.
 */
UCLASS()
class ALGONARTS_API UAlgonaPresentationSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<AAlgonaArmyPresentationActor> ArmyPresentationActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AAlgonaLegacyIsmPresentationActor> LegacyPresentationActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AAlgonaRTSCameraActor> CameraActor = nullptr;
};
