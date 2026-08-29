#pragma once

// Core Mass fragment definitions for authoritative soldier state.
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

/** Whether a soldier currently participates in the formation. */
UENUM()
enum class EAlgonaSoldierFormationState : uint8
{
	Active,
	Lost
};

/** Distance category used by squad cohesion rules. */
UENUM()
enum class EAlgonaSoldierDistanceState : uint8
{
	Near,
	Medium,
	Far,
	Lost
};

/** Reserved first-pass state hook for the later narrow-passage system. */
UENUM()
enum class EAlgonaSoldierPassageState : uint8
{
	Formation,
	WaitingForPassage,
	Passing,
	RejoiningFormation
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

/** Squad membership and current formation assignment. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSquadMemberFragment : public FMassFragment
{
	GENERATED_BODY()

	int32 SquadId = INDEX_NONE;
	int32 SlotIndex = INDEX_NONE;
	EAlgonaSoldierFormationState FormationState =
		EAlgonaSoldierFormationState::Active;

	// Reserved for future typed units/slots; zero is the ordinary first type.
	uint8 UnitType = 0;
};

/**
 * Authoritative movement state.
 * Idle/moving/lost soldiers remain the same Mass entity and archetype.
 */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierMovementFragment : public FMassFragment
{
	GENERATED_BODY()

	FVector Velocity = FVector::ZeroVector;
	uint64 LastProcessedSimulationTick = 0;
	EAlgonaSoldierMovementState State = EAlgonaSoldierMovementState::Idle;
	EAlgonaSoldierDistanceState DistanceState = EAlgonaSoldierDistanceState::Near;
	EAlgonaSoldierPassageState PassageState = EAlgonaSoldierPassageState::Formation;

	// Deterministic per-soldier variation. Values are derived from the stable
	// session ID, so movement remains fixed-step/multiplayer friendly.
	uint32 MotionVariationSeed = 0;
	float PersonalSpeedScale = 1.0f;
	float BodyTurnSpeedScale = 1.0f;

	// Per-command movement character is generated once, then reused. The current
	// side offset is persistent soldier state: stopping freezes it, and the next
	// order continues smoothly from that exact value instead of resetting to Slot.
	uint32 CommandVariationRevision = 0;
	float TravelDriftSegmentLengthCm = 2250.0f;
	float CommandStartSideOffsetCm = 0.0f;
	float CurrentSideOffsetCm = 0.0f;
};
