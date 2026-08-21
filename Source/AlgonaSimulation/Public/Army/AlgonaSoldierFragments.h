#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"

#include "AlgonaSoldierFragments.generated.h"

/** Игровое состояние движения солдата. */
UENUM()
enum class EAlgonaSoldierMovementState : uint8
{
	Idle,
	Moving
};

/** Маркер массовой боевой entity. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierTag : public FMassTag
{
	GENERATED_BODY()
};

/**
 * Стабильный ID внутри одной simulation session.
 * Это пока не финальный network ID для P3.
 */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierIdFragment : public FMassFragment
{
	GENERATED_BODY()

	uint32 Value = 0;
};

/** Хранит принадлежность солдата к отряду и его постоянное место в формации. */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSquadMemberFragment : public FMassFragment
{
	GENERATED_BODY()

	int32 SquadId = INDEX_NONE;
	int32 SlotIndex = INDEX_NONE;
};

/**
 * Игровое состояние движения солдата.
 * Стоящий и движущийся солдат остаются одной и той же Mass entity.
 */
USTRUCT()
struct ALGONASIMULATION_API FAlgonaSoldierMovementFragment : public FMassFragment
{
	GENERATED_BODY()

	FVector Velocity = FVector::ZeroVector;

	uint64 LastProcessedSimulationTick = 0;

	EAlgonaSoldierMovementState State =
		EAlgonaSoldierMovementState::Idle;
};