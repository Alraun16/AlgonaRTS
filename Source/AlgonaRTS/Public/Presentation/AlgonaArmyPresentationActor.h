#pragma once

#include "Army/AlgonaSoldierSnapshot.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaArmyPresentationActor.generated.h"

class UAlgonaSimulationSubsystem;
class UCameraComponent;
class UInstancedStaticMeshComponent;

/**
 * Один Actor отображает текущий локальный Presentation working set армии.
 * Simulation state по-прежнему полностью принадлежит AlgonaSimulation.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaArmyPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaArmyPresentationActor();

	virtual void Tick(float DeltaSeconds) override;

	void SetPresentationCamera(
		UCameraComponent* InCameraComponent);

private:
	void CaptureLatestState(
		UAlgonaSimulationSubsystem& Simulation);

	void RefreshPresentationWorkingSet(
		bool bSimulationChanged);

	bool HasSameEntityOrder(
		const TArray<uint32>& CandidateEntityIds) const;

	bool HasCameraViewChanged() const;
	void CacheCurrentCameraView();

	void RebuildInstances();

	void ApplyInterpolatedTransforms(
		double InterpolationAlpha);

	UPROPERTY(VisibleAnywhere, Category = "Algona|Presentation")
	TObjectPtr<UInstancedStaticMeshComponent> InstancedMeshComponent =
		nullptr;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	int32 MaxPresentedEntities = 500000;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	FVector InstanceScale = FVector(1.0, 1.0, 1.0);

	/*
	 * Полный renderer-neutral snapshot текущей Simulation.
	 * Здесь могут находиться все 100k.
	 */
	TArray<FAlgonaSoldierSnapshot> CachedSnapshots;

	/*
	 * Только локальный Presentation working set:
	 * те entities, которые сейчас нужны камере.
	 */
	TArray<uint32> PresentedEntityIds;

	TArray<FTransform> PreviousTransforms;
	TArray<FTransform> TargetTransforms;
	TArray<FTransform> RenderTransforms;

	/*
	 * Переиспользуемые временные массивы для следующего working set.
	 */
	TArray<uint32> NextPresentedEntityIds;
	TArray<FTransform> NextTargetTransforms;

	TWeakObjectPtr<UCameraComponent> PresentationCamera;

	FTransform LastCameraTransform = FTransform::Identity;

	uint64 LastStateRevision = 0;
	uint64 LastSimulationTick = 0;
	float VisibilityRefreshElapsedSeconds = 0.0f;
	float VisibilityRefreshIntervalSeconds = 0.05f;
	float LastCameraOrthoWidth = 0.0f;
	float LastCameraAspectRatio = 0.0f;

	bool bHasCapturedState = false;
	bool bInterpolationActive = false;
	bool bHasCameraView = false;

	bool bHasCullingMode = false;
	bool bLastCullingEnabled = true;
	
	float DebugCounterElapsedSeconds = 0.0f;
};