#pragma once

#include "CoreMinimal.h"

/**
 * P1 presentation mode selector.
 * Simulation never reads this value: it only controls local rendering.
 */
enum class EAlgonaP1PresentationMode : uint8
{
	SimulationOnly = 0,
	LegacyStaticIsm = 1,
	InstancedSkinnedMesh = 2
};

ALGONARTS_API EAlgonaP1PresentationMode GetAlgonaP1PresentationMode();
ALGONARTS_API bool IsAlgonaP1PresentationCameraCullingEnabled();
