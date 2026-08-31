#pragma once

#include "AlgonaFixedStepAccumulator.h"
#include "AlgonaSimulationStatus.h"
#include "Army/AlgonaSoldierSnapshot.h"
#include "Army/AlgonaSquad.h"
#include "Spatial/AlgonaSquadSpatialGrid.h"
#include "Spatial/AlgonaSoldierSpatialGrid.h"
#include "Spatial/AlgonaSquadSpatialSnapshot.h"

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
	inline constexpr double SpatialGridCellSizeCm = 10000.0;
}

/** Command consumed at the beginning of the next authoritative fixed step. */
struct FAlgonaSquadMoveCommand
{
	int32 SquadId = INDEX_NONE;
	FVector TargetLocation = FVector::ZeroVector;
};


enum class EAlgonaSpatialGridMode : uint8
{
	None = 0,
	Squads = 1,
	Soldiers = 2,
	Both = 3
};

/**
 * World-scoped authoritative Simulation Core.
 * It exists in standalone/server worlds, never depends on Presentation, and
 * updates gameplay state only through fixed simulation steps.
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

	double GetSpatialGridCellSizeCm() const
	{
		return SquadSpatialGrid.GetCellSizeCm();
	}

	FIntPoint GetSpatialGridCellCoordinates(const FVector& WorldPosition) const
	{
		return SquadSpatialGrid.GetCellCoordinates(WorldPosition);
	}

	FVector2D GetSpatialGridCellWorldMin(const FIntPoint& Cell) const
	{
		return SquadSpatialGrid.GetCellWorldMin(Cell);
	}

	FAlgonaSimulationMetrics GetSimulationMetrics() const;

	/**
	 * Full renderer-neutral export retained as a fail-open/debug fallback when
	 * spatial camera selection is disabled or unavailable.
	 */
	int32 ExportSoldierSnapshots(
		TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
		int32 MaxEntities);

	/** Returns only squad centers from spatial-grid cells intersecting bounds. */
	int32 QuerySquadsInBounds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<FAlgonaSquadSpatialSnapshot>& OutSquads) const;

	/**
	 * Exports complete selected squads without scanning unrelated soldiers.
	 * The caller decides why a squad is relevant; Simulation never sees camera
	 * or renderer state.
	 */
	int32 ExportSoldierSnapshotsForSquads(
		TConstArrayView<int32> SquadIds,
		TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
		int32 MaxEntities);
	
	int32 ExportSoldierSnapshotsForSoldierIds(
	TConstArrayView<uint32> SoldierIds,
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
	int32 MaxEntities);
	
#if !UE_BUILD_SHIPPING
	void BenchmarkSpatialGridQueries(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		int32 Iterations) const;

	void BenchmarkSpatialSnapshotPaths(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		int32 Iterations);
#endif
	
	bool SubmitMoveSquadCommand(
		int32 SquadId,
		const FVector& TargetLocation);

	int32 SubmitMoveAllSquadsByOffset(const FVector& Offset);

private:
	struct FAlgonaSquadEntityRange
	{
		int32 FirstSoldierIndex = INDEX_NONE;
		int32 Count = 0;
	};

	bool IsSquadSpatialGridEnabled() const
	{
		return SpatialGridMode == EAlgonaSpatialGridMode::Squads
			|| SpatialGridMode == EAlgonaSpatialGridMode::Both;
	}

	bool IsSoldierSpatialGridEnabled() const
	{
		return SpatialGridMode == EAlgonaSpatialGridMode::Soldiers
			|| SpatialGridMode == EAlgonaSpatialGridMode::Both;
	}
	
	void InitializeQueries();

	bool CreateSoldiers(
		int32 SoldierCount,
		int32 RequestedSquadSize);
	bool CreateSquads(int32 RequestedSquadSize);
	void DestroySoldiers();

	void RunSimulationStep(float DeltaTime);
	void ProcessPendingMoveCommands();
	bool UpdateSquadAnchors(
		float DeltaTime,
		int32& OutSpatialCellChanges);
	int32 UpdateSoldiers(
		float DeltaTime,
		int32& OutVisitedEntities,
		int32& OutSpatialCellChanges);

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

	FAlgonaSquadSpatialGrid SquadSpatialGrid{
		AlgonaSimulationDefaults::SpatialGridCellSizeCm};
	
	FAlgonaSoldierSpatialGrid SoldierSpatialGrid{
		AlgonaSimulationDefaults::SpatialGridCellSizeCm};

	EAlgonaSpatialGridMode SpatialGridMode =
		EAlgonaSpatialGridMode::Squads;

	bool bSpatialGridBenchmarkEnabled = false;

	uint64 SpatialBenchmarkStepCount = 0;
	double SpatialBenchmarkStepMillisecondsSum = 0.0;
	double SpatialBenchmarkStepMillisecondsMax = 0.0;

	uint64 SpatialBenchmarkSquadCellChanges = 0;
	uint64 SpatialBenchmarkSoldierCellChanges = 0;
	
	TArray<FMassEntityHandle> SoldierEntities;
	TArray<FAlgonaSquad> Squads;
	TArray<FAlgonaSquadEntityRange> SquadEntityRanges;
	TArray<FAlgonaSquadMoveCommand> PendingMoveCommands;
};
