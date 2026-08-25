#pragma once

#include "CoreMinimal.h"

/**
 * Authoritative state of one RTS squad.
 * Soldiers store their SquadId and SlotIndex in Mass fragments; the squad does
 * not duplicate a list of entity handles.
 */
struct ALGONASIMULATION_API FAlgonaSquad
{
	int32 GetFormationCapacity() const
	{
		return FormationWidth * FormationDepth;
	}

	int32 SquadId = INDEX_NONE;
	int32 MemberCount = 0;

	int32 FormationWidth = 10;
	int32 FormationDepth = 5;
	float SoldierSpacing = 80.0f;

	// Anchor is the front-center reference point of the formation.
	FVector AnchorLocation = FVector::ZeroVector;
	FVector TargetAnchorLocation = FVector::ZeroVector;
	FVector FacingDirection = FVector::ForwardVector;

	// P1 debug movement values. Gameplay data will replace these later.
	float AnchorMoveSpeed = 300.0f;
	float SoldierMoveSpeed = 450.0f;
	bool bHasMoveTarget = false;
};
