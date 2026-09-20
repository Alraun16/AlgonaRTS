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

	/** Ускорение центра Squad, см/с²: полная скорость за AccelerationSeconds. */
	float GetMoveAcceleration() const;

	/** Замедление центра Squad, см/с². */
	float GetMoveDeceleration() const;

	/** Угловое ускорение строя, рад/с²: полная угловая скорость за то же время. */
	float GetYawAcceleration() const;

	/**
	 * Максимальная скорость поворота строя, рад/с: крайний слот на дуге
	 * движется не быстрее TurnSpeedFactor * CenterMoveSpeed.
	 */
	float GetMaxYawRate() const;

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
	// появятся вместе с приказами движения. Для подбора скорости в редакторе
	// есть CVar algona.P2.SquadMoveSpeed.
	float CenterMoveSpeed = 450.0f;

	// Максимальная скорость Unit = CenterMoveSpeed * UnitSpeedFactor:
	// запас скорости на догон строя и перестроение.
	float UnitSpeedFactor = 1.3f;

	// То же, пока строй поворачивается. На ходу крайний слот при повороте
	// движется быстрее Squad (скорость центра + скорость от вращения),
	// поэтому Unit нужен больший запас.
	float TurningUnitSpeedFactor = 1.5f;

	bool bHasMoveTarget = false;

	// Конечное направление текущего приказа. Движение его не перезаписывает.
	FVector FinalFacingDirection = FVector::ForwardVector;

	// Приказ ещё требует поворота к FinalFacingDirection (в том числе после
	// прибытия центра).
	bool bHasFacingTarget = false;

	// Режим марша (длинный путь): Squad поворачивается по ходу движения и
	// поворачивается к конечному направлению только на подходе к цели.
	// На коротком пути Squad сразу поворачивается к конечному направлению
	// и идёт боком или спиной.
	bool bMarching = false;

	// Марш: поворот к конечному направлению на подходе уже начат.
	bool bFinalTurnStarted = false;

	// Плавный старт после зеркального разворота, с: пока Unit разворачиваются
	// на месте, скорость движения и поворота Squad растёт от нуля до полной.
	float MirrorTurnStartRemainingSeconds = 0.0f;

	// Текущая скорость поворота строя, рад/с (знак — направление). Меняется
	// с угловым ускорением и служит упреждением для L2, как CenterVelocity.
	float YawRate = 0.0f;

	// Текущая скорость центра вдоль пути, см/с. Меняется с ускорением.
	float CenterSpeed = 0.0f;

	// Скорость крайнего слота при повороте относительно заданной скорости
	// Squad. Меньше UnitSpeedFactor, чтобы у крайних Unit был запас на догон.
	float TurnSpeedFactor = 1.0f;

	// На ходу скорости центра и вращения складываются. Общая скорость
	// крайнего слота ограничена MaxSlotSpeedFactor от скорости Squad,
	// иначе крайние Unit бегут заметно быстрее остальных. Поворот при этом
	// не медленнее MinTurnRateFactor от максимальной угловой скорости.
	float MaxSlotSpeedFactor = 1.3f;
	float MinTurnRateFactor = 0.4f;

	// Длина строки из составного приказа «движение + ширина»; 0 — нет.
	// Применяется за RowLengthApplyDistanceCm до цели (или сразу, если ближе).
	int32 PendingRowLength = 0;

	static constexpr double RowLengthApplyDistanceCm = 1000.0;

	// Путь считается маршем, если он длиннее обоих порогов:
	// MarchMinDistanceCm и MarchMinRadiusFactor радиусов раскладки.
	static constexpr double MarchMinDistanceCm = 2000.0;
	static constexpr double MarchMinRadiusFactor = 2.0;

	// Марш: доля пути, на которой начинается доворот к конечному
	// направлению. 1.0 — доворот заканчивается ровно в цели, меньше —
	// Squad доворачивает позже и заканчивает уже на месте.
	static constexpr double FinalTurnDistanceFactor = 0.8;

	// Длительность плавного старта после зеркального разворота, с. Это не
	// второй разгон, а потолок скорости, пока Unit разворачиваются на месте.
	// Позже длительность будет браться из анимации разворота Unit.
	static constexpr float MirrorTurnStartSeconds = 0.5f;

	// Инерция: за сколько секунд Squad набирает полную скорость движения
	// и полную скорость поворота. Торможение во столько раз резче разгона
	// (1 — разгон и торможение одинаковые).
	static constexpr float AccelerationSeconds = 1.0f;
	static constexpr float DecelerationFactor = 1.0f;

	// То же для Unit: Unit разгоняется вдвое быстрее Squad, иначе не
	// догоняет слот.
	static constexpr float UnitAccelerationSeconds = 0.5f;
};
