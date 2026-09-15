#pragma once

#include "Army/AlgonaFormation.h"

#include "CoreMinimal.h"

/**
 * Authoritative state of one RTS squad.
 *
 * Раскладка слотов (где стоять) отделена от назначения (кто где стоит):
 * - FormationLayout строится генератором формы только при Reform;
 * - ActiveUnitIds хранит UnitId по номеру слота.
 * Unit хранит свои SquadId и SlotIndex во фрагменте. Связь проверяется
 * ValidateSquadMembership в Simulation Subsystem.
 */
struct ALGONASIMULATION_API FAlgonaSquad
{
	int32 SquadId = INDEX_NONE;

	// Поза Squad: центр раскладки слотов на земле и направление «вперёд».
	FVector CenterLocation = FVector::ZeroVector;
	FVector TargetCenterLocation = FVector::ZeroVector;
	FVector FacingDirection = FVector::ForwardVector;

	// Параметры построения — вход генератора формы.
	FAlgonaFormationParams FormationParams;

	// Раскладка слотов — выход генератора формы. Меняется только при Reform.
	FAlgonaFormationLayout FormationLayout;

	// Увеличивается при каждом изменении раскладки слотов.
	uint32 FormationRevision = 0;

	// Активные Unit по слотам: ActiveUnitIds[SlotIndex] = UnitId.
	TArray<uint32> ActiveUnitIds;

	// P1 debug movement values. Gameplay data will replace these later.
	float CenterMoveSpeed = 300.0f;
	float UnitMoveSpeed = 450.0f;
	bool bHasMoveTarget = false;
};
