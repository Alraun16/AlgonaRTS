#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaPresentationSubsystem.generated.h"

class AActor;
class AAlgonaArmyPresentationActor;
class AAlgonaArmySkinnedPresentationActor;

/**
 * Creates local army presentation only in worlds that need rendering.
 */
UCLASS()
class ALGONARTS_API UAlgonaPresentationSubsystem final
	: public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(
		UObject* Outer) const override;

	virtual void OnWorldBeginPlay(
		UWorld& InWorld) override;

	virtual void Deinitialize() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<AAlgonaArmyPresentationActor> PresentationActor =
		nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AAlgonaArmySkinnedPresentationActor> SkinnedPresentationActor =
		nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AActor> TestCameraActor =
		nullptr;
};
