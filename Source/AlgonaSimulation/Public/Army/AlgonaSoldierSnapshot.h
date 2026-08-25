#pragma once

#include "CoreMinimal.h"

/**
 * Minimal renderer-neutral state exported from Simulation to Presentation.
 * No Mass handle, renderer index, material, LOD or representation data leaks
 * across this boundary.
 */
struct ALGONASIMULATION_API FAlgonaSoldierSnapshot
{
	uint32 EntityId = 0;
	FVector Position = FVector::ZeroVector;
	FQuat Facing = FQuat::Identity;
};
