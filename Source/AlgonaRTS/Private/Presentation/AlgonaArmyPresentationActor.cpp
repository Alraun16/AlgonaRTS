#include "Presentation/AlgonaArmyPresentationActor.h"

#include "Core/AlgonaSimulationSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ConstructorHelpers.h"

AAlgonaArmyPresentationActor::AAlgonaArmyPresentationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	InstancedMeshComponent =
		CreateDefaultSubobject<UInstancedStaticMeshComponent>(
			TEXT("ArmyInstances"));
	SetRootComponent(InstancedMeshComponent);

	InstancedMeshComponent->SetMobility(EComponentMobility::Movable);
	InstancedMeshComponent->SetCollisionEnabled(
		ECollisionEnabled::NoCollision);
	InstancedMeshComponent->SetGenerateOverlapEvents(false);
	InstancedMeshComponent->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SoldierMesh(
		TEXT("/Game/Archer.Archer"));

	if (SoldierMesh.Succeeded())
	{
		InstancedMeshComponent->SetStaticMesh(SoldierMesh.Object);
	}
}

void AAlgonaArmyPresentationActor::Tick(float DeltaSeconds)
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

	const uint64 CurrentRevision = Simulation->GetStateRevision();

	if (!bHasCapturedState
		|| CurrentRevision != LastStateRevision)
	{
		CaptureLatestState(*Simulation);
		LastStateRevision = CurrentRevision;
		bHasCapturedState = true;
	}

	if (bInterpolationActive)
	{
		ApplyInterpolatedTransforms(
			DeltaSeconds,
			Simulation->GetFixedStepSeconds());
	}
}

void AAlgonaArmyPresentationActor::CaptureLatestState(
	UAlgonaSimulationSubsystem& Simulation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_CaptureState);
	
	const FQuat MeshFacingCorrection =
	FRotator(0.0f, -90.0f, 0.0f).Quaternion();
	
	Simulation.ExportSoldierSnapshots(
		CachedSnapshots,
		MaxPresentedEntities);

	if (!HasSameEntityOrder())
	{
		PresentedEntityIds.SetNumUninitialized(
			CachedSnapshots.Num());
		TargetTransforms.SetNumUninitialized(
			CachedSnapshots.Num());

		for (int32 Index = 0;
			Index < CachedSnapshots.Num();
			++Index)
		{
			const FAlgonaSoldierSnapshot& Snapshot =
				CachedSnapshots[Index];

			PresentedEntityIds[Index] = Snapshot.EntityId;
			
			const FQuat PresentationRotation =
				(Snapshot.Facing * MeshFacingCorrection).GetNormalized();

			TargetTransforms[Index] = FTransform(
				PresentationRotation,
				Snapshot.Position,
				InstanceScale);
		}

		PreviousTransforms = TargetTransforms;
		RenderTransforms = TargetTransforms;
		InterpolationElapsedSeconds = 0.0f;
		bInterpolationActive = false;

		RebuildInstances();
		return;
	}

	PreviousTransforms = RenderTransforms;
	bool bAnyTransformChanged = false;

	for (int32 Index = 0;
		Index < CachedSnapshots.Num();
		++Index)
	{
		const FAlgonaSoldierSnapshot& Snapshot =
			CachedSnapshots[Index];

		const FQuat PresentationRotation =
			(Snapshot.Facing * MeshFacingCorrection).GetNormalized();

		const FTransform NewTarget(
			PresentationRotation,
			Snapshot.Position,
			InstanceScale);

		if (!NewTarget.Equals(RenderTransforms[Index], 0.01f))
		{
			bAnyTransformChanged = true;
		}

		TargetTransforms[Index] = NewTarget;
	}

	InterpolationElapsedSeconds = 0.0f;
	bInterpolationActive = bAnyTransformChanged;
}

bool AAlgonaArmyPresentationActor::HasSameEntityOrder() const
{
	if (PresentedEntityIds.Num() != CachedSnapshots.Num())
	{
		return false;
	}

	for (int32 Index = 0;
		Index < CachedSnapshots.Num();
		++Index)
	{
		if (PresentedEntityIds[Index]
			!= CachedSnapshots[Index].EntityId)
		{
			return false;
		}
	}

	return true;
}

void AAlgonaArmyPresentationActor::RebuildInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_RebuildInstances);

	InstancedMeshComponent->ClearInstances();

	if (TargetTransforms.IsEmpty())
	{
		return;
	}

	InstancedMeshComponent->PreAllocateInstancesMemory(
		TargetTransforms.Num());

	InstancedMeshComponent->AddInstances(
		TargetTransforms,
		false,
		true,
		false);
}

void AAlgonaArmyPresentationActor::ApplyInterpolatedTransforms(
	float DeltaSeconds,
	double FixedStepSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaPresentation_InterpolateAndUpload);

	if (PreviousTransforms.Num() != TargetTransforms.Num()
		|| RenderTransforms.Num() != TargetTransforms.Num())
	{
		bInterpolationActive = false;
		return;
	}

	InterpolationElapsedSeconds += FMath::Max(
		DeltaSeconds,
		0.0f);

	const float SafeDuration = FMath::Max(
		static_cast<float>(FixedStepSeconds),
		KINDA_SMALL_NUMBER);

	const float Alpha = FMath::Clamp(
		InterpolationElapsedSeconds / SafeDuration,
		0.0f,
		1.0f);

	for (int32 Index = 0;
		Index < TargetTransforms.Num();
		++Index)
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

	if (Alpha >= 1.0f)
	{
		bInterpolationActive = false;
	}
}