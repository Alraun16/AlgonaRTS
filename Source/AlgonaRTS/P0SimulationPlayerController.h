#pragma once

#include "AlgonaSimulationTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "P0SimulationPlayerController.generated.h"

UCLASS()
class ALGONARTS_API AP0SimulationPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

	UFUNCTION(BlueprintCallable, Category = "Algona P0")
	void SubmitMoveGroupCommand(FSimGroupId GroupId, FVector TargetPosition);

	UFUNCTION(BlueprintCallable, Category = "Algona P0")
	void SubmitMoveAllGroupsCommand(FVector TargetPosition);

protected:
	UFUNCTION(Server, Reliable)
	void ServerSubmitMoveGroupCommand(int32 GroupId, FVector TargetPosition);

	UFUNCTION(Server, Reliable)
	void ServerSubmitMoveAllGroupsCommand(FVector TargetPosition);

private:
	void HandleDebugSelectGroup();
	void HandleDebugMoveSelectedGroup();
	bool IsDebugMouseModifierDown() const;
	bool ResolveDebugCursorWorldPosition(FVector& OutWorldPosition) const;

	FSimGroupId DebugSelectedGroup;
};
