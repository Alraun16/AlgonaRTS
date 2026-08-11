#pragma once

#include "AlgonaSimulationTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "P0SimulationCommandBridge.generated.h"

UCLASS()
class ALGONARTS_API AP0SimulationCommandBridge : public AActor
{
	GENERATED_BODY()

public:
	AP0SimulationCommandBridge();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void DumpDebugState() const;

private:
	void RefreshReplicatedSnapshot();

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	int32 ServerEntityCount = 0;

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	int32 ServerGroupCount = 0;

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	int32 ServerPendingCommandCount = 0;

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	uint64 ServerSimulationTick = 0;

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	uint64 ServerLastSubmittedCommandSequence = 0;

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	uint64 ServerLastAppliedCommandSequence = 0;

	UPROPERTY(Replicated, VisibleAnywhere, Category = "Algona P0")
	int32 ServerLastAppliedCommandGroupId = INDEX_NONE;
};
