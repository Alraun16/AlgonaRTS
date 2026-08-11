#include "P0SimulationPresentationActor.h"

#include "AlgonaSimulationSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaP0Presentation, Log, All);

namespace
{
AP0SimulationPresentationActor* FindP0PresentationActor(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<AP0SimulationPresentationActor> It(World); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

void DumpPresentationP0Command(UWorld* World)
{
	AP0SimulationPresentationActor* PresentationActor =
		FindP0PresentationActor(World);
	if (PresentationActor == nullptr)
	{
		UE_LOG(LogAlgonaP0Presentation, Warning, TEXT("algona.P0.DumpPresentation failed: no presentation actor."));
		return;
	}

	PresentationActor->DumpDebugState();
}

FAutoConsoleCommandWithWorld GAlgonaP0DumpPresentationCommand(
	TEXT("algona.P0.DumpPresentation"),
	TEXT("Dumps the compact P0 presentation actor state."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&DumpPresentationP0Command)
);
}

AP0SimulationPresentationActor::AP0SimulationPresentationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.05f;

	InstancedMeshComponent =
		CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("P0Instances"));
	SetRootComponent(InstancedMeshComponent);
	InstancedMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube")
	);
	if (CubeMesh.Succeeded())
	{
		InstanceMesh = CubeMesh.Object;
		InstancedMeshComponent->SetStaticMesh(InstanceMesh);
	}
}

void AP0SimulationPresentationActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	RefreshInstances();
}

void AP0SimulationPresentationActor::DumpDebugState() const
{
	UE_LOG(
		LogAlgonaP0Presentation,
		Log,
		TEXT("P0 presentation. Actor=%s, RefreshCount=%llu, Presented=%d, Instances=%d, LastExportMs=%.3f, LastRefreshMs=%.3f, MaxPresented=%d"),
		*GetNameSafe(this),
		RefreshCount,
		LastPresentedEntityCount,
		LastInstanceCount,
		LastExportMilliseconds,
		LastRefreshMilliseconds,
		MaxPresentedEntities
	);
}

void AP0SimulationPresentationActor::RefreshInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_PresentationUpdate);
	const double StartSeconds = FPlatformTime::Seconds();

	if (InstancedMeshComponent == nullptr || InstanceMesh == nullptr)
	{
		LastPresentedEntityCount = 0;
		LastInstanceCount = 0;
		LastExportMilliseconds = 0.0;
		LastRefreshMilliseconds = 0.0;
		return;
	}

	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		World != nullptr
			? World->GetSubsystem<UAlgonaSimulationSubsystem>()
			: nullptr;
	if (SimulationSubsystem == nullptr)
	{
		LastPresentedEntityCount = 0;
		LastInstanceCount = InstancedMeshComponent->GetNumInstances();
		LastExportMilliseconds = 0.0;
		LastRefreshMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return;
	}

	const double ExportStartSeconds = FPlatformTime::Seconds();
	SimulationSubsystem->ExportDebugPresentationEntities(
		CachedEntities,
		MaxPresentedEntities
	);
	LastExportMilliseconds =
		(FPlatformTime::Seconds() - ExportStartSeconds) * 1000.0;

	CachedTransforms.Reset(CachedEntities.Num());
	for (const FAlgonaDebugPresentationEntity& Entity : CachedEntities)
	{
		const float UniformScale = Entity.CubeSize > 0.0f
			? Entity.CubeSize / 100.0f
			: InstanceScale.X;
		FVector FacingDirection = Entity.FacingDirection;
		FacingDirection.Z = 0.0;
		const FQuat FacingRotation = FacingDirection.Normalize()
			? FacingDirection.Rotation().Quaternion()
			: FQuat::Identity;
		CachedTransforms.Emplace(
			FacingRotation,
			Entity.Position,
			FVector(UniformScale, UniformScale * 0.65f, UniformScale)
		);
	}

	if (InstancedMeshComponent->GetNumInstances() != CachedTransforms.Num())
	{
		InstancedMeshComponent->ClearInstances();
		InstancedMeshComponent->PreAllocateInstancesMemory(CachedTransforms.Num());
		InstancedMeshComponent->AddInstances(
			CachedTransforms,
			false,
			true,
			false
		);
		LastPresentedEntityCount = CachedEntities.Num();
		LastInstanceCount = InstancedMeshComponent->GetNumInstances();
		LastRefreshMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		++RefreshCount;
		return;
	}

	if (!CachedTransforms.IsEmpty())
	{
		InstancedMeshComponent->BatchUpdateInstancesTransforms(
			0,
			CachedTransforms,
			true,
			true,
			true
		);
	}

	LastPresentedEntityCount = CachedEntities.Num();
	LastInstanceCount = InstancedMeshComponent->GetNumInstances();
	LastRefreshMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	++RefreshCount;
}
