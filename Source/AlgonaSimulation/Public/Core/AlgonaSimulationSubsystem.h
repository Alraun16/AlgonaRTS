#pragma once

// Stable Simulation Core helpers and renderer-neutral exported data.
#include "AlgonaFixedStepAccumulator.h"
#include "AlgonaSimulationStatus.h"
#include "Army/AlgonaSoldierSnapshot.h"
#include "Army/AlgonaSquad.h"
#include "Spatial/AlgonaSquadSpatialGrid.h"
#include "Spatial/AlgonaSquadSpatialSnapshot.h"

// Unreal/Mass world subsystem infrastructure.
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
	inline constexpr double SpatialGridCellSizeCm = 5000.0;

	inline constexpr float NearFormationMarginCm = 500.0f;
	inline constexpr float MediumFormationMarginCm = 1000.0f;
	inline constexpr float LostRecoveryDistanceCm = 500.0f;
	inline constexpr float StragglerWaitSeconds = 5.0f;
	inline constexpr float FinalAssemblyTimeoutSeconds = 5.0f;
}

/** Command consumed at the beginning of the next authoritative fixed step. */
struct FAlgonaSquadMoveCommand
{
	int32 SquadId = INDEX_NONE;
	FVector TargetLocation = FVector::ZeroVector;
	EAlgonaSquadMovePace Pace = EAlgonaSquadMovePace::Run;

	// Zero final-facing means a normal click command: the squad finishes facing
	// the direction naturally produced by its route. A positive row length asks
	// Rectangle to Reform during the final 15 m of this accepted route.
	FVector FinalFacingDirection = FVector::ZeroVector;
	int32 RequestedMaxSlotsPerRow = 0;
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

	/** Minimal gameplay query used by selection/debug presentation. */
	bool GetSquadCenter(int32 SquadId, FVector& OutCenter) const;

	/** Formation data needed to preview a held-RMB command without exposing Mass. */
	bool GetSquadCommandPreview(
		int32 SquadId,
		FAlgonaSquadCommandPreview& OutPreview) const;

	/**
	 * Exports complete selected squads without scanning unrelated soldiers.
	 * The caller decides why a squad is relevant; Simulation never sees camera
	 * or renderer state.
	 */
	int32 ExportSoldierSnapshotsForSquads(
		TConstArrayView<int32> SquadIds,
		TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
		int32 MaxEntities);

	bool SubmitMoveSquadCommand(
		int32 SquadId,
		const FVector& TargetLocation,
		EAlgonaSquadMovePace Pace = EAlgonaSquadMovePace::Run,
		const FVector& FinalFacingDirection = FVector::ZeroVector,
		int32 RequestedMaxSlotsPerRow = 0);

	int32 SubmitMoveAllSquadsByOffset(const FVector& Offset);

private:
	/** Physical spawn range retained for the optimized P1 snapshot exporter. */
	struct FAlgonaSquadEntityRange
	{
		int32 FirstSoldierIndex = INDEX_NONE;
		int32 Count = 0;
	};

	void InitializeQueries();

	bool CreateSoldiers(
		int32 SoldierCount,
		int32 RequestedSquadSize);
	bool CreateSquads(int32 RequestedSquadSize);
	void DestroySoldiers();

	// Fixed-step command -> squad -> soldier update pipeline.
	void RunSimulationStep(float DeltaTime);
	void ProcessPendingMoveCommands();
	bool UpdateSquadAnchors(float DeltaTime);
	int32 UpdateSoldiers(
		float DeltaTime,
		int32& OutVisitedEntities);
	void EvaluateSquadCohesion(float DeltaTime);

	// Rare O(squad-size) structure operations. They never scan the whole army.
	void ReformSquad(FAlgonaSquad& Squad);
	void RefreshActiveMemberSlotFragments(FAlgonaSquad& Squad);
	void EnsureMirrorSlotMap(FAlgonaSquad& Squad);
	void MirrorActiveMemberAssignments(FAlgonaSquad& Squad);
	int32 MarkCurrentFarMembersLost(FAlgonaSquad& Squad);
	void ProcessRecoveredLostMembers(TConstArrayView<int32> SoldierIndices);
	void RestoreEmptySquadFromNearestLostMember(FAlgonaSquad& Squad);

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

	TArray<FMassEntityHandle> SoldierEntities;
	TArray<FAlgonaSquad> Squads;
	TArray<FAlgonaSquadEntityRange> SquadEntityRanges;
	TArray<FAlgonaSquadMoveCommand> PendingMoveCommands;

	// Reused fixed-step scratch storage: no per-soldier allocation in hot loop.
	TArray<int32> RecoveredLostSoldierIndices;
};
