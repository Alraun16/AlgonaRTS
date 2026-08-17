#pragma once

#include "AlgonaSquad.h"

#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaSimulationSubsystem.generated.h"

class UMassEntityConfigAsset;
class UMassEntitySubsystem;
class UMassSpawnerSubsystem;

/*
 World-scoped coordinator текущего P0-прототипа.

 Отвечает за fixed-step boundary, создание Mass soldiers и стартовую
 структуру squads. Реальная пакетная Simulation и отдельная Presentation
 добавляются следующим архитектурным этапом.
*/
UCLASS()
class ALGONASIMULATION_API UAlgonaSimulationSubsystem
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(
		FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	// Пока только фиксирует границу одного simulation step.
	void RunSimulationStep(float DeltaTime);

	// Одноразовый P0 spawn и стартовая раскладка.
	void CreateSoldiers();
	void CreateSquads();

	static constexpr float FixedSimulationStepSeconds =
		1.0f / 20.0f;
	static constexpr int32 P0SoldierCount = 20000;

	float SimulationAccumulatorSeconds = 0.0f;

	UMassEntitySubsystem* MassEntitySubsystem = nullptr;
	UMassSpawnerSubsystem* MassSpawnerSubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMassEntityConfigAsset> SoldierEntityConfig =
		nullptr;

	TArray<FMassEntityHandle> SoldierEntities;
	TArray<FAlgonaSquad> Squads;
};