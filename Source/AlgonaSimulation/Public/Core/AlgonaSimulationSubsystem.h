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

ALGONASIMULATION_API DECLARE_LOG_CATEGORY_EXTERN(LogAlgonaSimulation, Log, All);

namespace AlgonaSimulationDefaults
{
	inline constexpr double FixedStepSeconds = 1.0 / 40.0;
	inline constexpr int32 MaxStepsPerFrame = 5;
	inline constexpr int32 UnitCount = 20000;
	inline constexpr int32 SquadSize = 50;
	inline constexpr double SpatialGridCellSizeCm = 10000.0;

	// Стартовая раскладка армии: число отрядов в одном ряду вдоль X.
	inline constexpr int32 SquadsPerRow = 40;

	// Стресс-сценарий замеров P2: длина одного прохода ряда отрядов по Y, см.
	inline constexpr double StressMoveDistanceCm = 4000.0;

	// Dormant squad-level spatial index. Change only this value to re-enable
	// population and updates when squad-level spatial queries are needed again.
	inline constexpr bool EnableSquadSpatialGrid = false;
}

/** Тип команды Squad в очереди команд Simulation. */
enum class EAlgonaSquadCommandType : uint8
{
	// Идти центром Squad в точку. Заменяет текущий приказ движения.
	Move,

	// Приказ длины строки: Reform относительно центра Squad.
	SetRowLength
};

/**
 * Команда очереди команд Simulation.
 * Очередь — транспорт от игрока, сети и AI: все команды применяются строго
 * по порядку поступления в начале следующего fixed step, затем очередь
 * очищается. Текущий приказ хранит сам Squad.
 */
struct FAlgonaSquadCommand
{
	EAlgonaSquadCommandType Type = EAlgonaSquadCommandType::Move;
	int32 SquadId = INDEX_NONE;

	// Move: целевая точка центра Squad.
	FVector TargetLocation = FVector::ZeroVector;

	// SetRowLength: запрошенная длина строки.
	int32 RowLength = 0;
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

	// Команды не меняют состояние сразу: они ставятся в очередь и
	// применяются в начале следующего fixed step.
	bool SubmitMoveSquadCommand(
		int32 SquadId,
		const FVector& TargetLocation);

	bool SubmitSetRowLengthCommand(
		int32 SquadId,
		int32 RowLength);

	int32 SubmitMoveAllSquadsByOffset(const FVector& Offset);

	/**
	 * Инструмент замеров P2, не игровая механика.
	 * Соседние ряды отрядов постоянно ходят навстречу друг другу и
	 * разворачиваются по прибытии, поэтому двигаются все Unit.
	 * Приказы идут через обычную очередь команд. Включается консольной
	 * командой algona.P2.StressMove, которая есть только в не-shipping сборках.
	 */
	void SetStressMoveEnabled(bool bEnabled);

private:
	// Накопленная статистика шагов за одно окно реального времени.
	// Используется только для периодического отчёта в лог.
	struct FAlgonaMetricsReportWindow
	{
		double StartWallSeconds = 0.0;
		uint64 StartOverloadedFrameCount = 0;
		int32 StepCount = 0;
		int32 MaxStepsPerFrame = 0;
		double StepMillisecondsSum = 0.0;
		double StepMillisecondsMax = 0.0;
		double CommandsMillisecondsSum = 0.0;
		double SquadsMillisecondsSum = 0.0;
		double UnitsMillisecondsSum = 0.0;
		int64 MovedEntitiesSum = 0;
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

	/**
	 * Проверяет связь Squad <-> Unit: число активных Unit равно числу слотов,
	 * у каждого Unit правильные SquadId и SlotIndex, каждый Unit ровно в
	 * одном слоте. Вызывается после создания армии в не-shipping сборках.
	 */
	bool ValidateSquadMembership();

	void RunSimulationStep(float DeltaTime);
	void ProcessPendingCommands();
	bool UpdateSquadCenters(float DeltaTime);
	int32 UpdateUnits(
		float DeltaTime,
		int32& OutVisitedEntities);

	FVector ComputeSlotWorldPosition(
		const FAlgonaSquad& Squad,
		int32 SlotIndex) const;

	bool IsAuthoritativeSimulationWorld() const;

	void SubmitStressMoveCommands();

	void AccumulateMetricsReportStep();
	void UpdateMetricsReport(int32 ExecutedStepsThisFrame);

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
	// Очередь команд до начала следующего fixed step.
	TArray<FAlgonaSquadCommand> PendingCommands;

	FAlgonaMetricsReportWindow MetricsReportWindow;

	// Состояние стресс-сценария: направление следующего прохода по Y
	// (+1 или -1) для каждого SquadId.
	bool bStressMoveEnabled = false;
	TArray<int8> StressMoveDirections;
};
