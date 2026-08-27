#pragma once

#include "Army/AlgonaSoldierSnapshot.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaLegacyIsmPresentationActor.generated.h"

class UAlgonaSimulationSubsystem;
class UCameraComponent;
class UInstancedStaticMeshComponent;

/**
 * Legacy static-ISM presentation retained only as a stable A/B baseline.
 * It intentionally keeps the old CPU interpolation path.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaLegacyIsmPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaLegacyIsmPresentationActor();

	virtual void Tick(float DeltaSeconds) override;

	void SetPresentationCamera(UCameraComponent* InCameraComponent);

private:
	void CaptureLatestState(UAlgonaSimulationSubsystem& Simulation);
	void RefreshPresentationWorkingSet(bool bSimulationChanged);
	bool HasSameEntityOrder(const TArray<uint32>& CandidateEntityIds) const;
	bool HasCameraViewChanged() const;
	void CacheCurrentCameraView();
	void RebuildInstances();
	void ApplyInterpolatedTransforms(double InterpolationAlpha);

	UPROPERTY(VisibleAnywhere, Category = "Algona|Presentation")
	TObjectPtr<UInstancedStaticMeshComponent> InstancedMeshComponent = nullptr;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	int32 MaxPresentedEntities = 500000;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	FVector InstanceScale = FVector(1.0, 1.0, 1.0);

	TArray<FAlgonaSoldierSnapshot> CachedSnapshots;
	TArray<uint32> PresentedEntityIds;
	TArray<FTransform> PreviousTransforms;
	TArray<FTransform> TargetTransforms;
	TArray<FTransform> RenderTransforms;
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
	float DebugCounterElapsedSeconds = 0.0f;

	bool bHasCapturedState = false;
	bool bInterpolationActive = false;
	bool bHasCameraView = false;
	bool bHasCullingMode = false;
	bool bLastCullingEnabled = true;
	bool bHasSpatialSnapshotMode = false;
	bool bLastSpatialSnapshotsEnabled = true;
};
