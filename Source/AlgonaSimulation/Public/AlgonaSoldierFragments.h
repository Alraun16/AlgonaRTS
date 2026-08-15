#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"

#include "AlgonaSoldierFragments.generated.h"

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierTag : public FMassTag
{
	GENERATED_BODY()
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSquadMemberFragment : public FMassFragment
{
	GENERATED_BODY()

	int16 SquadId = INDEX_NONE;
};