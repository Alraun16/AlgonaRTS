#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"

#include "AlgonaSoldierFragments.generated.h"

/** Gameplay movement state of one authoritative soldier entity. */
UENUM()
enum class EAlgonaSoldierMovementState : uint8
{
	Idle,
	Moving
};

/** Marks an entity as an Algona combat soldier. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierTag : public FMassTag
{
	GENERATED_BODY()
};

/** Stable ID within one simulation session. Not the final P3 network ID. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierIdFragment : public FMassFragment
{
	GENERATED_BODY()

	uint32 Value = 0;
};

/** Squad membership and stable formation slot. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSquadMemberFragment : public FMassFragment
{
	GENERATED_BODY()

	int32 SquadId = INDEX_NONE;
	int32 SlotIndex = INDEX_NONE;
};

/**
 * Authoritative movement state.
 * Idle and moving soldiers remain the same Mass entity and archetype.
 */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierMovementFragment : public FMassFragment
{
	GENERATED_BODY()

	FVector Velocity = FVector::ZeroVector;
	uint64 LastProcessedSimulationTick = 0;
	EAlgonaSoldierMovementState State = EAlgonaSoldierMovementState::Idle;
};
