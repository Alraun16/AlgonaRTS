#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MassArchetypeTypes.h"
#include "Mass/EntityHandle.h"

#include "AlgonaSquad.h"

#include "AlgonaSimulationSubsystem.generated.h"

class UMassEntitySubsystem;

class UMassRepresentationSubsystem;
class UMassSpawnerSubsystem;
class UMassEntityConfigAsset;

/**
 * Central entry point for the Algona gameplay simulation.
 *
 * P0.1:
 * Tick -> RunSimulationStep -> simulation state / Mass.
 *
 * P0.2 will replace the direct per-frame simulation call
 * with a fixed-step accumulator.
 */
UCLASS()
class ALGONASIMULATION_API UAlgonaSimulationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PreDeinitialize() override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	void RunSimulationStep(float DeltaTime);
	static constexpr float FixedSimulationStepSeconds = 1.0f / 20.0f;

	float SimulationAccumulatorSeconds = 0.0f;
	
	UMassEntitySubsystem* MassEntitySubsystem = nullptr;
	UMassRepresentationSubsystem* MassRepresentationSubsystem = nullptr;
	
	UMassSpawnerSubsystem* MassSpawnerSubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMassEntityConfigAsset> SoldierEntityConfig = nullptr;
	
	FMassArchetypeHandle SoldierArchetype;
	
	void CreateSoldiers();
	void CreateSquads();
	
	static constexpr int32 P0SoldierCount = 20000;

	TArray<FMassEntityHandle> SoldierEntities;
	TArray<FAlgonaSquad> Squads;
	
	float RepresentationDebugElapsed = 0.0f;
	bool bRepresentationDebugLogged = false;
};