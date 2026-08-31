#pragma once

#include "AlgonaFixedStepAccumulator.h"
#include "AlgonaSimulationStatus.h"
#include "Army/AlgonaUnitSnapshot.h"
#include "Army/AlgonaSquad.h"
#include "Spatial/AlgonaSquadSpatialGrid.h"
#include "Spatial/AlgonaUnitSpatialGrid.h"
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
	inline constexpr int32 UnitCount = 20000;
	inline constexpr int32 SquadSize = 50;
	inline constexpr double SpatialGridCellSizeCm = 10000.0;

	// Dormant squad-level spatial index. Change only this value to re-enable
	// population and updates when squad-level spatial queries are needed again.
	inline constexpr bool EnableSquadSpatialGrid = false;
}

/** Command consumed at the beginning of the next authoritative fixed step. */
struct FAlgonaSquadMoveCommand
{
	int32 SquadId = INDEX_NONE;
	FVector TargetLocation = FVector::ZeroVector;
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

	int32 GetUnitCount() const
	{
		return UnitEntities.Num();
	}

	int32 GetSquadCount() const
	{
		return Squads.Num();
	}

	double GetUnitSpatialGridCellSizeCm() const
	{
		return UnitSpatialGrid.GetCellSizeCm();
	}

	FIntPoint GetUnitSpatialGridCellCoordinates(
		const FVector& WorldPosition) const
	{
		return UnitSpatialGrid.GetCellCoordinates(WorldPosition);
	}

	FVector2D GetUnitSpatialGridCellWorldMin(
		const FIntPoint& Cell) const
	{
		return UnitSpatialGrid.GetCellWorldMin(Cell);
	}

	FAlgonaSimulationMetrics GetSimulationMetrics() const;

	/**
	 * Full renderer-neutral export retained as a fail-open/debug fallback when
	 * spatial camera selection is disabled or unavailable.
	 */
	int32 ExportUnitSnapshots(
		TArray<FAlgonaUnitSnapshot>& OutSnapshots,
		int32 MaxEntities);

	/** Returns UnitIds from unit-grid cells intersecting the requested bounds. */
	int32 QueryUnitIdsInBounds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<uint32>& OutUnitIds) const;

	/**
	 * Dormant squad-level spatial query retained for future simulation systems.
	 * Returns no results while EnableSquadSpatialGrid is false.
	 */
	int32 QuerySquadsInBounds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<FAlgonaSquadSpatialSnapshot>& OutSquads) const;

	/**
	 * Exports complete selected squads. Retained with the dormant squad-grid
	 * path so squad-level spatial selection can be reconnected without redesign.
	 */
	int32 ExportUnitSnapshotsForSquads(
		TConstArrayView<int32> SquadIds,
		TArray<FAlgonaUnitSnapshot>& OutSnapshots,
		int32 MaxEntities);

	/** Exports only the units selected by renderer-neutral UnitIds. */
	int32 ExportUnitSnapshotsForUnitIds(
		TConstArrayView<uint32> UnitIds,
		TArray<FAlgonaUnitSnapshot>& OutSnapshots,
		int32 MaxEntities);

	bool SubmitMoveSquadCommand(
		int32 SquadId,
		const FVector& TargetLocation);

	int32 SubmitMoveAllSquadsByOffset(const FVector& Offset);

private:
	struct FAlgonaSquadEntityRange
	{
		int32 FirstUnitIndex = INDEX_NONE;
		int32 Count = 0;
	};

	bool IsSquadSpatialGridEnabled() const
	{
		return AlgonaSimulationDefaults::EnableSquadSpatialGrid;
	}

	void InitializeQueries();

	bool CreateUnits(
		int32 UnitCount,
		int32 RequestedSquadSize);
	bool CreateSquads(int32 RequestedSquadSize);
	void DestroyUnits();

	void RunSimulationStep(float DeltaTime);
	void ProcessPendingMoveCommands();
	bool UpdateSquadAnchors(float DeltaTime);
	int32 UpdateUnits(
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
	TObjectPtr<UMassEntityConfigAsset> UnitEntityConfig = nullptr;

	TUniquePtr<FMassEntityQuery> UnitUpdateQuery;
	TUniquePtr<FMassEntityQuery> UnitSnapshotQuery;

	// Kept dormant and empty while EnableSquadSpatialGrid is false.
	FAlgonaSquadSpatialGrid SquadSpatialGrid{
		AlgonaSimulationDefaults::SpatialGridCellSizeCm};

	// Active entity-level spatial index used by Presentation and future
	// simulation systems that need individual combat-unit queries.
	FAlgonaUnitSpatialGrid UnitSpatialGrid{
		AlgonaSimulationDefaults::SpatialGridCellSizeCm};

	TArray<FMassEntityHandle> UnitEntities;
	TArray<FAlgonaSquad> Squads;
	TArray<FAlgonaSquadEntityRange> SquadEntityRanges;
	TArray<FAlgonaSquadMoveCommand> PendingMoveCommands;
};
