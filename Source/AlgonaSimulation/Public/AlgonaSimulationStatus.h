#pragma once

#include "CoreMinimal.h"

enum class EAlgonaSimulationStartupState : uint8
{
	NotStarted,
	Ready,
	MissingMassServices,
	SoldierTemplateBuildFailed,
	SoldierSpawnFailed,
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
	double BacklogSeconds = 0.0;

	uint64 OverloadedFrameCount = 0;
};