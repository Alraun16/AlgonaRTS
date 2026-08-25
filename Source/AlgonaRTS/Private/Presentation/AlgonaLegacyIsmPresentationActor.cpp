#include "Presentation/AlgonaLegacyIsmPresentationActor.h"

#include "AlgonaPresentationView.h"
#include "Presentation/AlgonaPresentationSettings.h"
#include "Core/AlgonaSimulationSubsystem.h"

#include "Camera/CameraComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr double CullingGuardPixels = 128.0;
}

AAlgonaLegacyIsmPresentationActor::AAlgonaLegacyIsmPresentationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	InstancedMeshComponent =
		CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("LegacyArmyInstances"));
	SetRootComponent(InstancedMeshComponent);

	InstancedMeshComponent->SetMobility(EComponentMobility::Movable);
	InstancedMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InstancedMeshComponent->SetGenerateOverlapEvents(false);
	InstancedMeshComponent->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SoldierMesh(
		TEXT("/Game/Archer.Archer"));

	if (SoldierMesh.Succeeded())
	{
		InstancedMeshComponent->SetStaticMesh(SoldierMesh.Object);
	}
}

void AAlgonaLegacyIsmPresentationActor::SetPresentationCamera(
	UCameraComponent* InCameraComponent)
{
	PresentationCamera = InCameraComponent;
	bHasCameraView = false;
}

void AAlgonaLegacyIsmPresentationActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (!World || !InstancedMeshComponent)
	{
		return;
	}

	UAlgonaSimulationSubsystem* Simulation =
		World->GetSubsystem<UAlgonaSimulationSubsystem>();
	if (!Simulation)
	{
		return;
	}

	VisibilityRefreshElapsedSeconds += FMath::Max(DeltaSeconds, 0.0f);

	const uint64 CurrentSimulationTick = Simulation->GetSimulationTick();
	const uint64 CurrentRevision = Simulation->GetStateRevision();

	bool bSimulationChanged = false;
	if (!bHasCapturedState || CurrentRevision != LastStateRevision)
	{
		CaptureLatestState(*Simulation);
		LastStateRevision = CurrentRevision;
		bHasCapturedState = true;
		bSimulationChanged = true;
	}

	const bool bCullingEnabled =
		IsAlgonaP1PresentationCameraCullingEnabled();
	const bool bCullingModeChanged =
		!bHasCullingMode || bCullingEnabled != bLastCullingEnabled;
	const bool bCameraChanged = bCullingEnabled && HasCameraViewChanged();
	const bool bRefreshForCamera =
		bCameraChanged
		&& VisibilityRefreshElapsedSeconds >= VisibilityRefreshIntervalSeconds;

	if (bSimulationChanged || bCullingModeChanged || bRefreshForCamera)
	{
		RefreshPresentationWorkingSet(bSimulationChanged);
		CacheCurrentCameraView();
		VisibilityRefreshElapsedSeconds = 0.0f;
		bLastCullingEnabled = bCullingEnabled;
		bHasCullingMode = true;
	}

	const bool bSimulationAdvancedWithoutStateChange =
		!bSimulationChanged && CurrentSimulationTick != LastSimulationTick;

	if (bInterpolationActive)
	{
		if (bSimulationAdvancedWithoutStateChange)
		{
			RenderTransforms = TargetTransforms;
			if (!RenderTransforms.IsEmpty())
			{
				InstancedMeshComponent->BatchUpdateInstancesTransforms(
					0,
					RenderTransforms,
					true,
					true,
					true);
			}
			bInterpolationActive = false;
		}
		else
		{
			ApplyInterpolatedTransforms(Simulation->GetInterpolationAlpha());
		}
	}

	LastSimulationTick = CurrentSimulationTick;

#if !UE_BUILD_SHIPPING
	DebugCounterElapsedSeconds += FMath::Max(DeltaSeconds, 0.0f);
	if (DebugCounterElapsedSeconds >= 0.2f)
	{
		DebugCounterElapsedSeconds = 0.0f;
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				42001,
				0.25f,
				FColor::White,
				FString::Printf(
					TEXT("Presented Legacy ISM: %d   Simulation: %d"),
					InstancedMeshComponent->GetInstanceCount(),
					CachedSnapshots.Num()));
		}
	}
#endif
}

void AAlgonaLegacyIsmPresentationActor::CaptureLatestState(
	UAlgonaSimulationSubsystem& Simulation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaLegacyPresentation_CaptureState);
	Simulation.ExportSoldierSnapshots(CachedSnapshots, MaxPresentedEntities);
}

void AAlgonaLegacyIsmPresentationActor::RefreshPresentationWorkingSet(
	bool bSimulationChanged)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaLegacyPresentation_BuildCameraWorkingSet);

	NextPresentedEntityIds.Reset();
	NextTargetTransforms.Reset();
	NextPresentedEntityIds.Reserve(CachedSnapshots.Num());
	NextTargetTransforms.Reserve(CachedSnapshots.Num());

	FAlgonaPresentationView View;
	const UCameraComponent* Camera = PresentationCamera.Get();
	const bool bUseCameraCulling =
		IsAlgonaP1PresentationCameraCullingEnabled()
		&& Camera
		&& GetWorld()
		&& View.Build(*GetWorld(), *Camera);

	const FQuat MeshFacingCorrection =
		FRotator(0.0f, -90.0f, 0.0f).Quaternion();

	for (const FAlgonaSoldierSnapshot& Snapshot : CachedSnapshots)
	{
		if (bUseCameraCulling
			&& !View.IsGroundPointVisible(Snapshot.Position, CullingGuardPixels))
		{
			continue;
		}

		NextPresentedEntityIds.Add(Snapshot.EntityId);
		NextTargetTransforms.Emplace(
			(Snapshot.Facing * MeshFacingCorrection).GetNormalized(),
			Snapshot.Position,
			InstanceScale);
	}

	const bool bSameEntityOrder = HasSameEntityOrder(NextPresentedEntityIds);
	if (!bSameEntityOrder)
	{
		TMap<uint32, int32> OldIndexByEntityId;
		OldIndexByEntityId.Reserve(PresentedEntityIds.Num());

		for (int32 OldIndex = 0; OldIndex < PresentedEntityIds.Num(); ++OldIndex)
		{
			OldIndexByEntityId.Add(PresentedEntityIds[OldIndex], OldIndex);
		}

		TArray<FTransform> NewPreviousTransforms;
		TArray<FTransform> NewRenderTransforms;
		NewPreviousTransforms.SetNumUninitialized(NextPresentedEntityIds.Num());
		NewRenderTransforms.SetNumUninitialized(NextPresentedEntityIds.Num());

		bool bAnyTransformChanged = false;

		for (int32 NewIndex = 0; NewIndex < NextPresentedEntityIds.Num(); ++NewIndex)
		{
			const uint32 EntityId = NextPresentedEntityIds[NewIndex];
			const FTransform& NewTarget = NextTargetTransforms[NewIndex];
			const int32* OldIndexPtr = OldIndexByEntityId.Find(EntityId);

			if (OldIndexPtr
				&& PreviousTransforms.IsValidIndex(*OldIndexPtr)
				&& TargetTransforms.IsValidIndex(*OldIndexPtr)
				&& RenderTransforms.IsValidIndex(*OldIndexPtr))
			{
				const int32 OldIndex = *OldIndexPtr;
				NewRenderTransforms[NewIndex] = RenderTransforms[OldIndex];
				NewPreviousTransforms[NewIndex] =
					bSimulationChanged
						? TargetTransforms[OldIndex]
						: PreviousTransforms[OldIndex];

				if (!NewPreviousTransforms[NewIndex].Equals(NewTarget, 0.01f))
				{
					bAnyTransformChanged = true;
				}
			}
			else
			{
				// New camera entry has no valid previous visual state.
				NewPreviousTransforms[NewIndex] = NewTarget;
				NewRenderTransforms[NewIndex] = NewTarget;
			}
		}

		PresentedEntityIds = MoveTemp(NextPresentedEntityIds);
		PreviousTransforms = MoveTemp(NewPreviousTransforms);
		TargetTransforms = MoveTemp(NextTargetTransforms);
		RenderTransforms = MoveTemp(NewRenderTransforms);
		bInterpolationActive = bAnyTransformChanged;
		RebuildInstances();
		return;
	}

	if (!bSimulationChanged)
	{
		return;
	}

	PreviousTransforms = TargetTransforms;

	bool bAnyTransformChanged = false;
	for (int32 Index = 0; Index < NextTargetTransforms.Num(); ++Index)
	{
		if (!NextTargetTransforms[Index].Equals(TargetTransforms[Index], 0.01f))
		{
			bAnyTransformChanged = true;
		}
	}

	TargetTransforms = MoveTemp(NextTargetTransforms);
	bInterpolationActive = bAnyTransformChanged;
}

bool AAlgonaLegacyIsmPresentationActor::HasSameEntityOrder(
	const TArray<uint32>& CandidateEntityIds) const
{
	if (PresentedEntityIds.Num() != CandidateEntityIds.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < CandidateEntityIds.Num(); ++Index)
	{
		if (PresentedEntityIds[Index] != CandidateEntityIds[Index])
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaLegacyIsmPresentationActor::HasCameraViewChanged() const
{
	const UCameraComponent* Camera = PresentationCamera.Get();
	if (!Camera)
	{
		return false;
	}

	if (!bHasCameraView)
	{
		return true;
	}

	return !Camera->GetComponentTransform().Equals(LastCameraTransform, 0.01f)
		|| !FMath::IsNearlyEqual(Camera->OrthoWidth, LastCameraOrthoWidth, 0.01f)
		|| !FMath::IsNearlyEqual(Camera->AspectRatio, LastCameraAspectRatio, 0.001f);
}

void AAlgonaLegacyIsmPresentationActor::CacheCurrentCameraView()
{
	const UCameraComponent* Camera = PresentationCamera.Get();
	if (!Camera)
	{
		bHasCameraView = false;
		return;
	}

	LastCameraTransform = Camera->GetComponentTransform();
	LastCameraOrthoWidth = Camera->OrthoWidth;
	LastCameraAspectRatio = Camera->AspectRatio;
	bHasCameraView = true;
}

void AAlgonaLegacyIsmPresentationActor::RebuildInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaLegacyPresentation_RebuildInstances);

	InstancedMeshComponent->ClearInstances();
	if (RenderTransforms.IsEmpty())
	{
		return;
	}

	InstancedMeshComponent->PreAllocateInstancesMemory(RenderTransforms.Num());
	InstancedMeshComponent->AddInstances(RenderTransforms, false, true, false);
}

void AAlgonaLegacyIsmPresentationActor::ApplyInterpolatedTransforms(
	double InterpolationAlpha)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaLegacyPresentation_InterpolateAndUpload);

	if (PreviousTransforms.Num() != TargetTransforms.Num()
		|| RenderTransforms.Num() != TargetTransforms.Num())
	{
		bInterpolationActive = false;
		return;
	}

	const float Alpha = FMath::Clamp(
		static_cast<float>(InterpolationAlpha),
		0.0f,
		1.0f);

	for (int32 Index = 0; Index < TargetTransforms.Num(); ++Index)
	{
		const FVector Location = FMath::Lerp(
			PreviousTransforms[Index].GetLocation(),
			TargetTransforms[Index].GetLocation(),
			Alpha);

		const FQuat Rotation = FQuat::Slerp(
			PreviousTransforms[Index].GetRotation(),
			TargetTransforms[Index].GetRotation(),
			Alpha).GetNormalized();

		RenderTransforms[Index] = FTransform(
			Rotation,
			Location,
			TargetTransforms[Index].GetScale3D());
	}

	if (!RenderTransforms.IsEmpty())
	{
		InstancedMeshComponent->BatchUpdateInstancesTransforms(
			0,
			RenderTransforms,
			true,
			true,
			true);
	}
}
