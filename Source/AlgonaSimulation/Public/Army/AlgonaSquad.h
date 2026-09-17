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
	/** Направление «вперёд» на плоскости; ForwardVector, если оно не задано. */
	FVector GetForwardDirection2D() const;

	// Операции над составом меняют только массивы Squad. Вызывающий код
	// обязан обновить SlotIndex во фрагменте Unit, чей слот изменился,
	// и затем вызвать ReformAfterCompositionChange.

	/**
	 * Убирает активный Unit из слота SlotIndex (swap-remove): на его место
	 * встаёт последний активный Unit. OutMovedUnitId — UnitId, получивший
	 * слот SlotIndex, или 0, если убран последний Unit.
	 */
	bool RemoveActiveUnitAt(int32 SlotIndex, uint32& OutMovedUnitId);

	/** Добавляет Unit в конец состава. Возвращает его SlotIndex. */
	int32 AddActiveUnit(uint32 UnitId);

	/**
	 * Строит раскладку заново под текущий состав и параметры: пересчитывает
	 * длину строки по умолчанию (если игрок её не задавал) и увеличивает
	 * FormationRevision. Центр Squad не трогает.
	 */
	void RebuildFormationLayout();

	/**
	 * Reform после изменения состава (смерть, Потерянный, возвращение,
	 * подкрепление): строки остаются на месте, центр Squad сдвигается.
	 */
	void ReformAfterCompositionChange();

	/**
	 * Приказ игрока «длина строки»: Reform относительно центра, центр Squad
	 * остаётся на месте. Возвращает true, если раскладка изменилась.
	 */
	bool ApplyRowLengthOrder(int32 RowLength);

	int32 SquadId = INDEX_NONE;

	// Поза Squad: центр раскладки слотов на земле и направление «вперёд».
	FVector CenterLocation = FVector::ZeroVector;
	FVector TargetCenterLocation = FVector::ZeroVector;
	FVector FacingDirection = FVector::ForwardVector;

	// Параметры построения — вход генератора формы.
	FAlgonaFormationParams FormationParams;

	// Игрок задал длину строки вручную: правило по умолчанию больше не действует.
	bool bRowLengthSetByPlayer = false;

	// Раскладка слотов — выход генератора формы. Меняется только при Reform.
	FAlgonaFormationLayout FormationLayout;

	// Увеличивается при каждом изменении раскладки слотов.
	uint32 FormationRevision = 0;

	// Активные Unit по слотам: ActiveUnitIds[SlotIndex] = UnitId.
	TArray<uint32> ActiveUnitIds;

	// Радиус Unit («размер» Unit), см: круги выбора, позже — локальное избегание L3.
	// Пока один на Squad; у разных типов существ (лучник, тролль) будет свой.
	float UnitRadius = 35.0f;

	// Скорость центра за последний тик — упреждение для L2; ноль, если Squad стоит.
	FVector CenterVelocity = FVector::ZeroVector;

	// Заданная скорость Squad, см/с. Отдельные скорости ходьбы и бега
	// появятся вместе с приказами движения.
	float CenterMoveSpeed = 300.0f;

	// Максимальная скорость Unit = CenterMoveSpeed * UnitSpeedFactor:
	// запас скорости на догон строя и перестроение.
	float UnitSpeedFactor = 1.25f;

	bool bHasMoveTarget = false;

	// Конечное направление текущего приказа движения: Squad принимает его
	// по прибытии. Движение его не перезаписывает.
	FVector FinalFacingDirection = FVector::ForwardVector;

	// Длина строки из составного приказа «движение + ширина»; 0 — нет.
	// Применяется за RowLengthApplyDistanceCm до цели (или сразу, если ближе).
	int32 PendingRowLength = 0;

	static constexpr double RowLengthApplyDistanceCm = 1000.0;
};
