#pragma once

#include "CoreMinimal.h"

enum class EAlgonaSimulationStartupState : uint8
{
	NotStarted,
	Ready,
	MissingMassServices,
	UnitTemplateBuildFailed,
	UnitSpawnFailed,
	SquadInitializationFailed
};

struct ALGONASIMULATION_API FAlgonaSimulationMetrics
{
	EAlgonaSimulationStartupState StartupState =
		EAlgonaSimulationStartupState::NotStarted;

	uint64 SimulationTick = 0;
	int32 EntityCount = 0;
	int32 SquadCount = 0;
	int32 LastVisitedEntities = 0;
	int32 LastMovedEntities = 0;
	int32 LastExecutedStepsThisFrame = 0;
	double LastStepMilliseconds = 0.0;

	// Время отдельных стадий последнего fixed step, мс.
	// Сумма стадий чуть меньше LastStepMilliseconds: остаток — учёт метрик.
	double LastCommandsMilliseconds = 0.0;
	double LastSquadsMilliseconds = 0.0;

	// Движение Unit: L2 над плоскими массивами и обновление Unit Grid.
	double LastSteerMilliseconds = 0.0;
	double LastUnitGridMilliseconds = 0.0;

	// L3: перестройка мелкой сетки соседей и расталкивание.
	double LastLocalGridMilliseconds = 0.0;
	double LastSeparationMilliseconds = 0.0;

	// Конвейер движения последнего шага выполнялся на рабочих потоках.
	bool bLastParallelMovement = false;
	double BacklogSeconds = 0.0;
	uint64 OverloadedFrameCount = 0;
};
