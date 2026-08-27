#pragma once

#include "Army/AlgonaSoldierSnapshot.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InstanceDataTypes.h"

#include "AlgonaArmyPresentationActor.generated.h"

class UAlgonaSimulationSubsystem;
class UAnimBank;
class UAnimSequenceTransformProviderData;
class UCameraComponent;
class UInstancedSkinnedMeshComponent;
class UMaterialInterface;
class USceneComponent;

/** Screen-significance tier used only by local Presentation interpolation. */
enum class EAlgonaPresentationInterpolationTier : uint8
{
	Near = 0,
	Medium = 1,
	Far = 2
};

/**
 * Primary P1 army presentation.
 *
 * Simulation remains authoritative at 40 Hz. This actor owns only local
 * camera visibility, ISKM instances, animation and GPU buffered interpolation.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaArmyPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaArmyPresentationActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	void SetPresentationCamera(UCameraComponent* InCameraComponent);

private:
	bool PrepareSkinnedRenderer();
	UAnimSequenceTransformProviderData* CreateSequenceProvider(UAnimBank* InAnimBank);
	bool ValidateInstancingBuildSettings();
	bool BuildGpuInterpolationMaterials();

	void CaptureLatestState(UAlgonaSimulationSubsystem& Simulation);
	void RefreshPresentationWorkingSet(
		bool bSimulationChanged,
		bool bForceSnapshotSnap,
		double SimulationStepSeconds);

	bool HasSameEntityOrder(const TArray<uint32>& CandidateEntityIds) const;
	bool HasCameraViewChanged() const;
	void CacheCurrentCameraView();

	EAlgonaPresentationInterpolationTier DetermineInterpolationTier(
		float LinearSpeedCmPerSecond,
		EAlgonaPresentationInterpolationTier PreviousTier,
		bool bHasPreviousTier) const;

	bool ShouldSnapInterpolation(
		const FTransform& Previous,
		const FTransform& Current) const;

	bool RebuildInstances();
	bool UploadSimulationStateToInstances();
	bool UploadInstanceGpuData(int32 Index);

	void BeginBufferedInterpolation();
	void CompleteBufferedInterpolation();
	void UpdateGpuInterpolationAlphas(
		double InterpolationAlpha,
		float DeltaSeconds);
	void PushGpuInterpolationAlphas();
	void RefreshInterpolationTierPresence();

	void Fail(const FString& Message);
	void Succeed(const FString& Message);

	UPROPERTY(VisibleAnywhere, Category = "Algona|Presentation")
	TObjectPtr<USceneComponent> SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Algona|Presentation")
	TObjectPtr<UInstancedSkinnedMeshComponent> InstancedSkinnedMeshComponent = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimBank> AnimBank = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequenceTransformProviderData> SequenceProvider = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> RuntimeInterpolationMaterials;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	int32 MaxPresentedEntities = 500000;

	UPROPERTY(EditAnywhere, Category = "Algona|Presentation")
	FVector InstanceScale = FVector(1.0, 1.0, 1.0);

	TArray<FAlgonaSoldierSnapshot> CachedSnapshots;
	TArray<uint32> PresentedEntityIds;
	TArray<FTransform> PreviousTransforms;
	TArray<FTransform> CurrentTransforms;
	TArray<float> ObservedLinearSpeedsCmPerSecond;
	TArray<EAlgonaPresentationInterpolationTier> InterpolationTiers;

	TArray<uint32> NextPresentedEntityIds;
	TArray<FTransform> NextCurrentTransforms;
	TArray<float> NextObservedLinearSpeedsCmPerSecond;
	TArray<EAlgonaPresentationInterpolationTier> NextInterpolationTiers;

	TArray<FPrimitiveInstanceId> InstanceIds;
	TArray<int32> AnimationIndicesScratch;

	TWeakObjectPtr<UCameraComponent> PresentationCamera;
	FTransform LastCameraTransform = FTransform::Identity;

	uint64 LastStateRevision = 0;
	uint64 LastSimulationTick = 0;

	float VisibilityRefreshElapsedSeconds = 0.0f;
	float VisibilityRefreshIntervalSeconds = 0.05f;
	float LastCameraOrthoWidth = 0.0f;
	float LastCameraAspectRatio = 0.0f;
	float DebugCounterElapsedSeconds = 0.0f;

	float NearInterpolationAlpha = 1.0f;
	float MediumInterpolationAlpha = 1.0f;

	// P1 screen-significance cache. Unit-height source will move to real
	// representation bounds when final production meshes are introduced.
	float ProjectedUnitHeightPixels = 0.0f;
	float GroundPixelsPerWorldUnit = 0.0f;
	float MaxMediumObservedSpeedCmPerSecond = 0.0f;
	double LastSimulationStepSeconds = 0.0;
	int32 MediumFrameCadence = 1;
	int32 MediumFramesSinceAlphaUpdate = 0;
	bool bHasNearInterpolationTier = false;
	bool bHasMediumInterpolationTier = false;

	int32 ProviderReadyFrames = 0;

	bool bHasCapturedState = false;
	bool bInterpolationActive = false;
	bool bHasCameraView = false;
	bool bHasCullingMode = false;
	bool bLastCullingEnabled = true;
	bool bHasSpatialSnapshotMode = false;
	bool bLastSpatialSnapshotsEnabled = true;
	bool bRendererReady = false;
	bool bRendererFailed = false;
};
