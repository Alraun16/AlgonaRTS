#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "P0TestGameMode.generated.h"

class AP0SimulationCommandBridge;
class AP0SimulationPresentationActor;

UCLASS()
class ALGONARTS_API AP0TestGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AP0TestGameMode();

	virtual void StartPlay() override;

	static AP0SimulationPresentationActor* FindPresentationActor(UWorld* World);
	static AP0SimulationPresentationActor* EnsurePresentationActor(UWorld* World);

private:
	UPROPERTY()
	TObjectPtr<AP0SimulationCommandBridge> CommandBridge;

	UPROPERTY()
	TObjectPtr<AP0SimulationPresentationActor> PresentationActor;
};
