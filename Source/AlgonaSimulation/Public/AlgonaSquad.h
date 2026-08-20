#pragma once

#include "CoreMinimal.h"

/**
 * Данные одного RTS-отряда.
 * Список солдат отдельно здесь не храним: каждый солдат сам знает свой SquadId.
 */
struct ALGONASIMULATION_API FAlgonaSquad
{
	int32 GetFormationCapacity() const
	{
		return FormationWidth * FormationDepth;
	}

	int32 SquadId = INDEX_NONE;
	int32 MemberCount = 0;

	// 100 солдат на отряд дают примерно 200 отрядов при 20 000 солдатах.
	int32 FormationWidth = 10;
	int32 FormationDepth = 5;
	float SoldierSpacing = 80.0f;

	// Опорная точка передней части строя.
	FVector AnchorLocation = FVector::ZeroVector;
	FVector FacingDirection = FVector::ForwardVector;
};