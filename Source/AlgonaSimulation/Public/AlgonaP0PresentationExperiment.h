#pragma once

#include "CoreMinimal.h"

/**
 * Temporary army presentation backend selector used for direct A/B tests.
 * Remove it after the production presentation path is chosen.
 */
enum class EAlgonaP0PresentationBackend : uint8
{
	LegacyMassStationary = 0,
	IsmCandidate = 1,
	SimulationOnly = 2,
	InstancedSkinnedMeshCandidate = 3
};

ALGONASIMULATION_API EAlgonaP0PresentationBackend
GetAlgonaP0PresentationBackend();
