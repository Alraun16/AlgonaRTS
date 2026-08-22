#pragma once

#include "AlgonaFixedStepAccumulator.h"
#include "AlgonaSimulationStatus.h"
#include "Army/AlgonaSoldierSnapshot.h"
#include "Army/AlgonaSquad.h"

#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"
#include "MassEntityQuery.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaSimulationSubsystem.generated.h"

class UMassEntityConfigAsset;
class UMassEntitySubsystem;
class UMassSpawnerSubsystem;

namespace AlgonaSimulationDefaults
{
	inline constexpr double FixedStepSeconds = 1.0 / 40.0;
	inline constexpr int32 MaxStepsPerFrame = 5;
	inline constexpr int32 SoldierCount = 20000;
	inline constexpr int32 SquadSize = 50;
}

/** Приказ, который будет применён в начале следующего fixed-step. */
struct FAlgonaSquadMoveCommand
{
	int32 SquadId = INDEX_NONE;
	FVector TargetLocation = FVector::ZeroVector;
};

/**
 * Главная Simulation-система текущего мира.
 * На обычном сетевом клиенте не создаётся.
 * Все игровые обновления выполняются через фиксированные simulation steps.
 */
UCLASS()
class ALGONASIMULATION_API UAlgonaSimulationSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	uint64 GetSimulationTick() const
	{
		return SimulationTick;
	}

	uint64 GetStateRevision() const
	{
		return StateRevision;
	}

	double GetFixedStepSeconds() const
	{
		return FixedStepAccumulator.GetFixedStepSeconds();
	}
	
	double GetInterpolationAlpha() const
	{
		return FixedStepAccumulator.GetInterpolationAlpha();
	}
	
	int32 GetSoldierCount() const
	{
		return SoldierEntities.Num();
	}

	int32 GetSquadCount() const
	{
		return Squads.Num();
	}

	FAlgonaSimulationMetrics GetSimulationMetrics() const;

	/**
	 * Копирует минимальные данные солдат для Presentation:
	 * ID, позицию и направление.
	 * Внутренние Mass handles и данные renderer наружу не передаются.
	 */
	int32 ExportSoldierSnapshots(
		TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
		int32 MaxEntities);
	
	// Приказ "простейшее движение"
	bool SubmitMoveSquadCommand(
	int32 SquadId,
	const FVector& TargetLocation);

	int32 SubmitMoveAllSquadsByOffset(
		const FVector& Offset);

private:
	void InitializeQueries();

	bool CreateSoldiers(
		int32 SoldierCount,
		int32 RequestedSquadSize);

	bool CreateSquads(int32 RequestedSquadSize);

	void DestroySoldiers();

	void RunSimulationStep(float DeltaTime);
	void ProcessPendingMoveCommands();
	bool UpdateSquadAnchors(float DeltaTime);
	int32 UpdateSoldiers(
		float DeltaTime,
		int32& OutVisitedEntities);

	FVector ComputeSlotWorldPosition(
		const FAlgonaSquad& Squad,
		int32 SlotIndex) const;

	bool IsAuthoritativeSimulationWorld() const;

	FAlgonaFixedStepAccumulator FixedStepAccumulator{
		AlgonaSimulationDefaults::FixedStepSeconds,
		AlgonaSimulationDefaults::MaxStepsPerFrame};

	uint64 SimulationTick = 0;
	uint64 StateRevision = 0;

	FAlgonaSimulationMetrics Metrics;

	UPROPERTY(Transient)
	TObjectPtr<UMassEntitySubsystem> MassEntitySubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMassSpawnerSubsystem> MassSpawnerSubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMassEntityConfigAsset> SoldierEntityConfig = nullptr;

	TUniquePtr<FMassEntityQuery> SoldierUpdateQuery;
	TUniquePtr<FMassEntityQuery> SoldierSnapshotQuery;

	TArray<FMassEntityHandle> SoldierEntities;
	TArray<FAlgonaSquad> Squads;
	TArray<FAlgonaSquadMoveCommand> PendingMoveCommands;
};