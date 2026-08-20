#pragma once

#include "CoreMinimal.h"

/**
 * Минимальные данные солдата, которые Simulation отдаёт Presentation.
 * Здесь нет Mass handles, ISM index, материалов, LOD и другой визуальной информации.
 */
struct ALGONASIMULATION_API FAlgonaSoldierSnapshot
{
	uint32 EntityId = 0;
	FVector Position = FVector::ZeroVector;
	FQuat Facing = FQuat::Identity;
};