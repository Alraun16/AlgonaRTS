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
#include "MassMovableVisualizationTrait.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationProcessor.h"
#include "MassStationaryVisualizationTrait.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"

#include "MassSpawnerSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaSimulation, Log, All);

void UAlgonaSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UWorld* World = GetWorld();
	MassEntitySubsystem = World
		? World->GetSubsystem<UMassEntitySubsystem>()
		: nullptr;
	
	MassRepresentationSubsystem =
	World->GetSubsystem<UMassRepresentationSubsystem>();
	
	MassSpawnerSubsystem =
	World->GetSubsystem<UMassSpawnerSubsystem>();
	
	if (MassEntitySubsystem && MassSpawnerSubsystem)
	{
		CreateSoldiers();
		CreateSquads();
	}
	
	/*
	if (MassEntitySubsystem)
	{
		FMassEntityManager& EntityManager =
			MassEntitySubsystem->GetMutableEntityManager();

		const UScriptStruct* SoldierElements[] =
		{
			FTransformFragment::StaticStruct(),
			FAlgonaSoldierTag::StaticStruct(),
			FAlgonaSquadMemberFragment::StaticStruct(),

			FMassRepresentationFragment::StaticStruct(),
			FMassRepresentationLODFragment::StaticStruct(),
			FMassVisualizationProcessorTag::StaticStruct()
		};

		SoldierArchetype = EntityManager.CreateArchetype(
			SoldierElements,
			FMassArchetypeCreationParams(FName(TEXT("AlgonaSoldier")))
		);
		
		CreateSoldiers();
		CreateSquads();
	}
	*/
	
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Initialized. World=%s, MassSubsystem=%s"),
		*GetNameSafe(World),
		*GetNameSafe(MassEntitySubsystem)
	);
}

void UAlgonaSimulationSubsystem::PreDeinitialize()
{
	Super::PreDeinitialize();
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Deinitialized.")
	);
	
	SoldierArchetype.Reset();
	
	MassEntitySubsystem = nullptr;
	MassRepresentationSubsystem = nullptr;
	
	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	SimulationAccumulatorSeconds += DeltaTime;

	while (SimulationAccumulatorSeconds >= FixedSimulationStepSeconds)
	{
		RunSimulationStep(FixedSimulationStepSeconds);
		SimulationAccumulatorSeconds -= FixedSimulationStepSeconds;
	}
	
	if (!bRepresentationDebugLogged &&
	MassEntitySubsystem &&
	!SoldierEntities.IsEmpty())
	{
		RepresentationDebugElapsed += DeltaTime;

		if (RepresentationDebugElapsed >= 1.0f)
		{
			FMassEntityManager& EntityManager =
				MassEntitySubsystem->GetMutableEntityManager();

			const FMassEntityHandle Soldier = SoldierEntities[0];

			if (EntityManager.IsEntityValid(Soldier))
			{
				FMassEntityView EntityView(EntityManager, Soldier);
				

				
				const FMassRepresentationFragment* Representation =
					EntityView.GetFragmentDataPtr<FMassRepresentationFragment>();

				const FMassRepresentationLODFragment* RepresentationLOD =
					EntityView.GetFragmentDataPtr<FMassRepresentationLODFragment>();

				if (Representation && RepresentationLOD)
				{
					const bool bValidMeshHandle =
						Representation->StaticMeshDescHandle.IsValid();

					const bool bHasISMData =
						MassRepresentationSubsystem &&
						bValidMeshHandle &&
						MassRepresentationSubsystem
							->GetISMCSharedDataForDescriptionIndex(
								Representation->StaticMeshDescHandle.ToIndex()
							) != nullptr;

					UE_LOG(
						LogAlgonaSimulation,
						Warning,
						TEXT(
							"Representation debug: "
							"Type=%d, "
							"MeshHandleValid=%d, "
							"MeshHandle=%d, "
							"ISMData=%d, "
							"LOD=%d, "
							"Significance=%.2f, "
							"Visibility=%d"
						),
						static_cast<int32>(
							Representation->CurrentRepresentation
						),
						bValidMeshHandle ? 1 : 0,
						bValidMeshHandle
							? Representation->StaticMeshDescHandle.ToIndex()
							: -1,
						bHasISMData ? 1 : 0,
						static_cast<int32>(
							RepresentationLOD->LOD.GetValue()
						),
						RepresentationLOD->LODSignificance,
						static_cast<int32>(
							RepresentationLOD->Visibility
						)
					);
				}
			}

			bRepresentationDebugLogged = true;
		}
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
	
	// P0.1 intentionally does not simulate soldiers yet.
	// P0.3 will introduce Mass soldier entities and their state.
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
		UE_LOG(
			LogAlgonaSimulation,
			Error,
			TEXT("Failed to load Archer mesh.")
		);

		return;
	}

	SoldierEntityConfig =
		NewObject<UMassEntityConfigAsset>(
			this,
			TEXT("AlgonaSoldierRuntimeConfig")
		);

	UAlgonaSoldierTrait* SoldierTrait =
	NewObject<UAlgonaSoldierTrait>(
		SoldierEntityConfig
	);

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
		UE_LOG(
			LogAlgonaSimulation,
			Error,
			TEXT("Failed to create soldier Mass traits.")
		);

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
	
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Soldier visualization: Meshes=%d, Mesh=%s"),
		VisualizationTrait->StaticMeshInstanceDesc.Meshes.Num(),
		*GetNameSafe(SoldierMesh)
	);
	
	const FMassEntityTemplate& SoldierTemplate =
		SoldierEntityConfig->GetOrCreateEntityTemplate(*World);

	SoldierEntities.Reset();
	SoldierEntities.Reserve(P0SoldierCount);

	MassSpawnerSubsystem->SpawnEntities(
		SoldierTemplate,
		P0SoldierCount,
		SoldierEntities
	);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Created %d Mass soldiers."),
		SoldierEntities.Num()
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

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Created %d squads from %d soldiers."),
		Squads.Num(),
		SoldierEntities.Num()
	);
}