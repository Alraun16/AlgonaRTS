#pragma once

#include "Army/AlgonaSoldierSnapshot.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaArmyPresentationActor.generated.h"

class UAlgonaSimulationSubsystem;
class UInstancedStaticMeshComponent;

/**
 * Один Actor рисует сразу всю армию через множество instances.
 * Игровые данные солдат по-прежнему принадлежат AlgonaSimulation.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaArmyPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaArmyPresentationActor();
	virtual void Tick(float DeltaSeconds) override;

private:
	void CaptureLatestState(UAlgonaSimulationSubsystem& Simulation);
	bool HasSameEntityOrder() const;
	void RebuildInstances();
	void ApplyInterpolatedTransforms(
		float DeltaSeconds,
		double FixedStepSeconds);

	UPROPERTY(VisibleAnywhere, Category = "Algona|Presentation")
	TObjectPtr<UInstancedStaticMeshComponent> InstancedMeshComponent =
		nullptr;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	int32 MaxPresentedEntities = 500000;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	FVector InstanceScale = FVector(1, 1, 1);

	TArray<FAlgonaSoldierSnapshot> CachedSnapshots;
	TArray<uint32> PresentedEntityIds;
	TArray<FTransform> PreviousTransforms;
	TArray<FTransform> TargetTransforms;
	TArray<FTransform> RenderTransforms;

	uint64 LastStateRevision = 0;
	float InterpolationElapsedSeconds = 0.0f;
	bool bHasCapturedState = false;
	bool bInterpolationActive = false;
};