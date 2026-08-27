#pragma once

#include "CoreMinimal.h"

/**
 * Minimal renderer-neutral squad position exposed to spatial consumers.
 * Camera, renderer and representation data never cross into Simulation.
 */
struct ALGONASIMULATION_API FAlgonaSquadSpatialSnapshot
{
	int32 SquadId = INDEX_NONE;
	FVector Center = FVector::ZeroVector;
};
