#pragma once

#include "CoreMinimal.h"

/**
 * Временный выбор способа отображения армии для сравнения.
 * После выбора рабочего варианта этот enum удаляется.
 */
enum class EAlgonaP0PresentationBackend : uint8
{
	LegacyMassStationary = 0,
	IsmCandidate = 1,
	SimulationOnly = 2
};

ALGONASIMULATION_API EAlgonaP0PresentationBackend
GetAlgonaP0PresentationBackend();