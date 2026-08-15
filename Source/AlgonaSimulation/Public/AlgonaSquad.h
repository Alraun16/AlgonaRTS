#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"

struct ALGONASIMULATION_API FAlgonaSquad
{
	int16 SquadId = INDEX_NONE;
	
	int16 FormationWidth = 10;
	int16 FormationDepth = 5;
	
	FVector AnchorLocation = FVector::ZeroVector;
	
	TArray<FMassEntityHandle> Members;
};