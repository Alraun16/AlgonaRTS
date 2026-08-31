#pragma once

#include "CoreMinimal.h"

/**
 * Authoritative state of one RTS squad.
 * Units store their SquadId and SlotIndex in Mass fragments; the squad does
 * not duplicate a list of entity handles.
 */
struct ALGONASIMULATION_API FAlgonaSquad
{
	int32 GetFormationCapacity() const
	{
		return FormationWidth * FormationDepth;
	}

	/** Center of the currently occupied formation footprint on the ground. */
	FVector GetSpatialCenter() const
	{
		if (MemberCount <= 0 || FormationWidth <= 0)
		{
			return AnchorLocation;
		}

		FVector Forward = FacingDirection.GetSafeNormal2D();
		if (Forward.IsNearlyZero())
		{
			Forward = FVector::ForwardVector;
		}

		const int32 UsedDepth = FMath::DivideAndRoundUp(
			MemberCount,
			FormationWidth);
		const double HalfOccupiedDepth =
			static_cast<double>(FMath::Max(UsedDepth - 1, 0))
			* static_cast<double>(UnitSpacing)
			* 0.5;

		return AnchorLocation - Forward * HalfOccupiedDepth;
	}

	int32 SquadId = INDEX_NONE;
	int32 MemberCount = 0;

	int32 FormationWidth = 10;
	int32 FormationDepth = 5;
	float UnitSpacing = 80.0f;

	// Anchor is the front-center reference point of the formation.
	FVector AnchorLocation = FVector::ZeroVector;
	FVector TargetAnchorLocation = FVector::ZeroVector;
	FVector FacingDirection = FVector::ForwardVector;

	// P1 debug movement values. Gameplay data will replace these later.
	float AnchorMoveSpeed = 300.0f;
	float UnitMoveSpeed = 450.0f;
	bool bHasMoveTarget = false;
};
