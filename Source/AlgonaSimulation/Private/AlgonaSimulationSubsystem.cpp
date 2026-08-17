#include "AlgonaSimulationSubsystem.h"

#include "AlgonaSoldierFragments.h"
#include "AlgonaSoldierTrait.h"

#include "Engine/StaticMesh.h"
#include "Engine/World.h"

#include "Mass/EntityFragments.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassEntityView.h"

#include "MassLODTypes.h"
#include "MassLODTrait.h"

#include "MassStationaryVisualizationTrait.h"
#include "MassRepresentationTypes.h"

#include "MassSpawnerSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"

void UAlgonaSimulationSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	MassEntitySubsystem =
		World->GetSubsystem<UMassEntitySubsystem>();
	MassSpawnerSubsystem =
		World->GetSubsystem<UMassSpawnerSubsystem>();

	if (MassEntitySubsystem && MassSpawnerSubsystem)
	{
		CreateSoldiers();
		CreateSquads();
	}
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	// Локальные handles/config не должны переживать world teardown.
	SoldierEntities.Reset();
	Squads.Reset();
	SoldierEntityConfig = nullptr;

	MassSpawnerSubsystem = nullptr;
	MassEntitySubsystem = nullptr;
	SimulationAccumulatorSeconds = 0.0f;

	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	SimulationAccumulatorSeconds += DeltaTime;

	while (SimulationAccumulatorSeconds
		>= FixedSimulationStepSeconds)
	{
		RunSimulationStep(FixedSimulationStepSeconds);
		SimulationAccumulatorSeconds -=
			FixedSimulationStepSeconds;
	}
}

TStatId UAlgonaSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UAlgonaSimulationSubsystem,
		STATGROUP_Tickables
	);
}

void UAlgonaSimulationSubsystem::RunSimulationStep(float DeltaTime)
{
	if (!MassEntitySubsystem)
	{
		return;
	}
	
	// Реальная пакетная обработка 20 000 entities добавляется следующим
	// отдельным P0-этапом. Здесь пока сохраняется fixed-step boundary.
}

void UAlgonaSimulationSubsystem::CreateSoldiers()
{
	UWorld* World = GetWorld();

	if (!World || !World->IsGameWorld() || !MassSpawnerSubsystem)
	{
		return;
	}

	UStaticMesh* SoldierMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Game/Archer.Archer")
	);

	if (!SoldierMesh)
	{
		return;
	}

	SoldierEntityConfig =
		NewObject<UMassEntityConfigAsset>(
			this,
			TEXT("AlgonaSoldierRuntimeConfig")
		);

	UAlgonaSoldierTrait* SoldierTrait =
		NewObject<UAlgonaSoldierTrait>(
			SoldierEntityConfig);

	UMassStationaryVisualizationTrait* VisualizationTrait =
		NewObject<UMassStationaryVisualizationTrait>(
			SoldierEntityConfig
		);

	UMassLODCollectorTrait* LODCollectorTrait =
		NewObject<UMassLODCollectorTrait>(
			SoldierEntityConfig
		);

	if (!SoldierTrait || !VisualizationTrait || !LODCollectorTrait)
	{
		return;
	}

	VisualizationTrait->StaticMeshInstanceDesc.Reset();

	FMassStaticMeshInstanceVisualizationMeshDesc& MeshDesc =
		VisualizationTrait->StaticMeshInstanceDesc.Meshes.AddDefaulted_GetRef();

	MeshDesc.Mesh = SoldierMesh;
	MeshDesc.ISMComponentClass =
		UInstancedStaticMeshComponent::StaticClass();

	MeshDesc.Mobility = EComponentMobility::Movable;
	MeshDesc.bCastShadows = false;

	MeshDesc.SetSignificanceRange(
		EMassLOD::High,
		EMassLOD::Off
	);

	VisualizationTrait->Params.LODRepresentation[EMassLOD::High] =
		EMassRepresentationType::StaticMeshInstance;

	VisualizationTrait->Params.LODRepresentation[EMassLOD::Medium] =
		EMassRepresentationType::StaticMeshInstance;

	VisualizationTrait->Params.LODRepresentation[EMassLOD::Low] =
		EMassRepresentationType::StaticMeshInstance;

	VisualizationTrait->Params.LODRepresentation[EMassLOD::Off] =
		EMassRepresentationType::None;
	
	VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::High] =
	0.0f;

	VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::Medium] =
		10000.0f;

	VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::Low] =
		30000.0f;

	VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::Off] =
		1000000.0f;

	VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::High] =
		0.0f;

	VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::Medium] =
		10000.0f;

	VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::Low] =
		30000.0f;

	VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::Off] =
		1000000.0f;
	
	VisualizationTrait->LODParams.LODMaxCount[EMassLOD::High] =
	P0SoldierCount;

	VisualizationTrait->LODParams.LODMaxCount[EMassLOD::Medium] =
		P0SoldierCount;

	VisualizationTrait->LODParams.LODMaxCount[EMassLOD::Low] =
		P0SoldierCount;

	VisualizationTrait->LODParams.LODMaxCount[EMassLOD::Off] =
		P0SoldierCount;
	
	VisualizationTrait->Params.ComputeCachedValues();

	FMassEntityConfig& SoldierConfig =
		SoldierEntityConfig->GetMutableConfig();

	SoldierConfig.AddTrait(*SoldierTrait);
	SoldierConfig.AddTrait(*VisualizationTrait);
	SoldierConfig.AddTrait(*LODCollectorTrait);
	
	const FMassEntityTemplate& SoldierTemplate =
		SoldierEntityConfig->GetOrCreateEntityTemplate(*World);

	SoldierEntities.Reset();
	SoldierEntities.Reserve(P0SoldierCount);

	MassSpawnerSubsystem->SpawnEntities(
		SoldierTemplate,
		P0SoldierCount,
		SoldierEntities
	);
}

void UAlgonaSimulationSubsystem::CreateSquads()
{
	if (!MassEntitySubsystem || SoldierEntities.IsEmpty())
	{
		return;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	constexpr float SoldierSpacing = 100.0f;
	constexpr float SquadSpacing = 1500.0f;
	constexpr int32 SquadsPerRow = 20;
	
	Squads.Reset();

	FAlgonaSquad DefaultSquad;

	const int32 SquadCapacity =
		static_cast<int32>(DefaultSquad.FormationWidth) *
		static_cast<int32>(DefaultSquad.FormationDepth);

	const int32 SquadCount =
		SoldierEntities.Num() / SquadCapacity;

	Squads.Reserve(SquadCount);

	int32 SoldierIndex = 0;

	for (int32 SquadIndex = 0; SquadIndex < SquadCount; ++SquadIndex)
	{
		FAlgonaSquad Squad;

		Squad.SquadId = static_cast<int16>(SquadIndex);
		
		const int32 SquadX = SquadIndex % SquadsPerRow;
		const int32 SquadY = SquadIndex / SquadsPerRow;

		Squad.AnchorLocation = FVector(
			SquadX * SquadSpacing,
			SquadY * SquadSpacing,
			0.0f
		);
		
		Squad.Members.Reserve(SquadCapacity);

		for (int32 MemberIndex = 0; MemberIndex < SquadCapacity; ++MemberIndex)
		{
			const FMassEntityHandle SoldierEntity =
				SoldierEntities[SoldierIndex++];

			Squad.Members.Add(SoldierEntity);

			FMassEntityView EntityView(EntityManager, SoldierEntity);

			FTransformFragment& TransformFragment =
				EntityView.GetFragmentData<FTransformFragment>();

			const int32 FormationX =
				MemberIndex % Squad.FormationWidth;

			const int32 FormationY =
				MemberIndex / Squad.FormationWidth;

			FTransform Transform = FTransform::Identity;

			Transform.SetLocation(
				Squad.AnchorLocation +
				FVector(
					FormationX * SoldierSpacing,
					FormationY * SoldierSpacing,
					0.0f
				)
			);

			TransformFragment.SetTransform(Transform);
			
			FAlgonaSquadMemberFragment& SquadMemberFragment =
				EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();

			SquadMemberFragment.SquadId = Squad.SquadId;
		}

		Squads.Add(MoveTemp(Squad));
	}
}