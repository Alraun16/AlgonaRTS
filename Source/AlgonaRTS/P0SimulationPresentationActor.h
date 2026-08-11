#pragma once

#include "CoreMinimal.h"
#include "AlgonaSimulationTypes.h"
#include "GameFramework/Actor.h"

#include "P0SimulationPresentationActor.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;

UCLASS()
class ALGONARTS_API AP0SimulationPresentationActor : public AActor
{
	GENERATED_BODY()

public:
	AP0SimulationPresentationActor();

	virtual void Tick(float DeltaSeconds) override;

	void DumpDebugState() const;
	void RefreshInstances();

private:
	UPROPERTY(VisibleAnywhere, Category = "Algona P0")
	TObjectPtr<UInstancedStaticMeshComponent> InstancedMeshComponent;

	UPROPERTY(EditAnywhere, Category = "Algona P0")
	TObjectPtr<UStaticMesh> InstanceMesh;

	UPROPERTY(EditAnywhere, Category = "Algona P0")
	int32 MaxPresentedEntities = 20000;

	UPROPERTY(EditAnywhere, Category = "Algona P0")
	FVector InstanceScale = FVector(0.25, 0.25, 0.25);

	TArray<FAlgonaDebugPresentationEntity> CachedEntities;
	TArray<FTransform> CachedTransforms;

	uint64 RefreshCount = 0;
	int32 LastPresentedEntityCount = 0;
	int32 LastInstanceCount = 0;
	double LastExportMilliseconds = 0.0;
	double LastRefreshMilliseconds = 0.0;
};
