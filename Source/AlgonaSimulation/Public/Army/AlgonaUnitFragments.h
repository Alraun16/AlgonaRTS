#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"

#include "AlgonaUnitFragments.generated.h"

/*
 * Mass-сущность Unit хранит только идентичность. Состояние Unit (позиция,
 * поворот, скорость, SquadId, SlotIndex) хранится в плоских массивах
 * Simulation Subsystem — это источник истины для движения (ADR P2, шаг 6a).
 */

/** Marks an entity as an Algona combat unit. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaUnitTag : public FMassTag
{
	GENERATED_BODY()
};

/** Stable ID within one simulation session. Not the final P3 network ID. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaUnitIdFragment : public FMassFragment
{
	GENERATED_BODY()

	uint32 Value = 0;
};
