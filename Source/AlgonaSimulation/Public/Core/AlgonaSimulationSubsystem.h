#pragma once

#include "AlgonaFixedStepAccumulator.h"
#include "AlgonaSimulationStatus.h"
#include "Army/AlgonaUnitSnapshot.h"
#include "Army/AlgonaSquad.h"
#include "Spatial/AlgonaTorusGrid.h"
#include "Spatial/AlgonaUnitSpatialGrid.h"

#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaSimulationSubsystem.generated.h"

class UMassEntityConfigAsset;
class UMassEntitySubsystem;
class UMassSpawnerSubsystem;

ALGONASIMULATION_API DECLARE_LOG_CATEGORY_EXTERN(LogAlgonaSimulation, Log, All);

namespace AlgonaSimulationDefaults
{
	inline constexpr double FixedStepSeconds = 1.0 / 40.0;
	// Сколько шагов симуляции разрешено прогнать в одном кадре, догоняя
	// отставание. Больше — симуляция точнее держит реальное время, но в
	// тяжёлой сцене кадр считает несколько шагов подряд и кадры проседают.
	// Два: при перегрузке симуляция идёт чуть медленнее реального времени,
	// зато картинка остаётся плавной.
	inline constexpr int32 MaxStepsPerFrame = 2;
	inline constexpr int32 UnitCount = 20000;
	inline constexpr int32 SquadSize = 50;
	inline constexpr double SpatialGridCellSizeCm = 10000.0;

	// Стресс-сценарий замеров P2: длина одного прохода ряда отрядов по Y, см,
	// и ширина полосы, в которой отряды идут в одну сторону.
	inline constexpr double StressMoveDistanceCm = 4000.0;
	inline constexpr double StressBandWidthCm = 2000.0;

	// Групповой приказ: расстояние между точками центров Squad относительно
	// размера самого крупного Squad группы.
	inline constexpr double GroupSpacingFactor = 1.1;
}

/** Тип команды Squad в очереди команд Simulation. */
enum class EAlgonaSquadCommandType : uint8
{
	// Идти центром Squad в точку. Заменяет текущий приказ движения.
	Move,

	// Приказ длины строки: Reform относительно центра Squad.
	SetRowLength,

	// Групповой приказ: несколько Squad в одну точку. Точки центров
	// раскладывает Simulation.
	MoveGroup
};

/**
 * Команда очереди команд Simulation.
 * Очередь — транспорт от игрока, сети и AI: все команды применяются строго
 * по порядку поступления в начале следующего fixed step, затем очередь
 * очищается. Текущий приказ хранит сам Squad.
 */
struct FAlgonaSquadCommand
{
	EAlgonaSquadCommandType Type = EAlgonaSquadCommandType::Move;
	int32 SquadId = INDEX_NONE;

	// Move, MoveGroup: целевая точка центра Squad (центра группы).
	FVector TargetLocation = FVector::ZeroVector;

	// Move: конечное направление. Нулевой вектор — направление последнего
	// отрезка пути (пока путь прямой — от центра Squad к цели).
	FVector FinalDirection = FVector::ZeroVector;

	// SetRowLength: запрошенная длина строки.
	// Move: длина строки составного приказа; 0 — не менять.
	int32 RowLength = 0;

	// MoveGroup: Squad группы.
	TArray<int32> SquadIds;
};

/**
 * Итог одного спавна армии: что реально встало на карту.
 * Клетка раскладки пропускается, если она не целиком на проходимой земле
 * или в ней уже стоят чужие Unit.
 */
struct FAlgonaSpawnArmyResult
{
	int32 UnitCount = 0;
	int32 SquadCount = 0;
	int32 SkippedCells = 0;

	// Свободных клеток не нашлось, спавн остановлен раньше времени.
	bool bRanOutOfSpace = false;
};

/**
 * World-scoped authoritative Simulation Core.
 * It exists in standalone/server worlds, never depends on Presentation, and
 * updates gameplay state only through fixed simulation steps.
 */
UCLASS()
class ALGONASIMULATION_API UAlgonaSimulationSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	uint64 GetSimulationTick() const
	{
		return SimulationTick;
	}

	uint64 GetStateRevision() const
	{
		return StateRevision;
	}

	double GetFixedStepSeconds() const
	{
		return FixedStepAccumulator.GetFixedStepSeconds();
	}

	double GetInterpolationAlpha() const
	{
		return FixedStepAccumulator.GetInterpolationAlpha();
	}

	int32 GetUnitCount() const
	{
		return UnitEntities.Num();
	}

	int32 GetSquadCount() const
	{
		return Squads.Num();
	}

	// Чтение состояния для локального интерфейса игрока (выбор, подсветка).
	// Только чтение: изменения идут через очередь команд.

	/** Радиус тела Unit, см; 0 — такого Unit нет. */
	float GetUnitRadius(uint32 UnitId) const
	{
		const int32 UnitIndex = static_cast<int32>(UnitId) - 1;
		return UnitState.Radii.IsValidIndex(UnitIndex)
			? UnitState.Radii[UnitIndex]
			: 0.0f;
	}

	/**
	 * Ворота коридора последнего построенного пути, парами точек
	 * (algona.P2.DebugPath 2). Только для отладочной отрисовки.
	 */
	const TArray<FVector>& GetDebugPathPortals() const
	{
		return DebugPathPortals;
	}

	/** Squad по SquadId или nullptr. */
	const FAlgonaSquad* FindSquad(int32 SquadId) const
	{
		return Squads.IsValidIndex(SquadId) && Squads[SquadId].SquadId == SquadId
			? &Squads[SquadId]
			: nullptr;
	}

	/** Позиция Unit из состояния Simulation. */
	bool GetUnitPosition(uint32 UnitId, FVector& OutPosition) const
	{
		const int32 UnitIndex = static_cast<int32>(UnitId) - 1;
		if (UnitId == 0 || !UnitState.Positions.IsValidIndex(UnitIndex))
		{
			return false;
		}

		OutPosition = UnitState.Positions[UnitIndex];
		return true;
	}

	/** SquadId Unit или INDEX_NONE. */
	int32 GetUnitSquadId(uint32 UnitId) const
	{
		const int32 UnitIndex = static_cast<int32>(UnitId) - 1;
		return UnitId != 0 && UnitState.SquadIds.IsValidIndex(UnitIndex)
			? UnitState.SquadIds[UnitIndex]
			: INDEX_NONE;
	}

	double GetUnitSpatialGridCellSizeCm() const
	{
		return UnitSpatialGrid.GetCellSizeCm();
	}

	FIntPoint GetUnitSpatialGridCellCoordinates(
		const FVector& WorldPosition) const
	{
		return UnitSpatialGrid.GetCellCoordinates(WorldPosition);
	}

	FVector2D GetUnitSpatialGridCellWorldMin(
		const FIntPoint& Cell) const
	{
		return UnitSpatialGrid.GetCellWorldMin(Cell);
	}

	FAlgonaSimulationMetrics GetSimulationMetrics() const;

	/**
	 * Full renderer-neutral export retained as a fail-open/debug fallback when
	 * spatial camera selection is disabled or unavailable.
	 */
	int32 ExportUnitSnapshots(
		TArray<FAlgonaUnitSnapshot>& OutSnapshots,
		int32 MaxEntities);

	/** Returns UnitIds from unit-grid cells intersecting the requested bounds. */
	int32 QueryUnitIdsInBounds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<uint32>& OutUnitIds) const;

	/**
	 * Exports complete selected squads. Retained with the dormant squad-grid
	 * path so squad-level spatial selection can be reconnected without redesign.
	 */
	int32 ExportUnitSnapshotsForSquads(
		TConstArrayView<int32> SquadIds,
		TArray<FAlgonaUnitSnapshot>& OutSnapshots,
		int32 MaxEntities);

	/** Exports only the units selected by renderer-neutral UnitIds. */
	int32 ExportUnitSnapshotsForUnitIds(
		TConstArrayView<uint32> UnitIds,
		TArray<FAlgonaUnitSnapshot>& OutSnapshots,
		int32 MaxEntities);

	// Команды не меняют состояние сразу: они ставятся в очередь и
	// применяются в начале следующего fixed step.
	bool SubmitMoveSquadCommand(
		int32 SquadId,
		const FVector& TargetLocation,
		const FVector& FinalDirection = FVector::ZeroVector,
		int32 RowLength = 0);

	bool SubmitMoveGroupCommand(
		TConstArrayView<int32> SquadIds,
		const FVector& TargetLocation);

	bool SubmitSetRowLengthCommand(
		int32 SquadId,
		int32 RowLength);

	int32 SubmitMoveAllSquadsByOffset(const FVector& Offset);

	/**
	 * Досыпает армию к уже существующей: создаёт Unit и расставляет Squad
	 * клетками от точки Origin назад по направлению Forward (Origin —
	 * середина передней линии). Клетка занимается только если площадка
	 * Squad целиком на проходимой земле и в ней нет чужих Unit.
	 */
	FAlgonaSpawnArmyResult SpawnArmy(
		int32 UnitCount,
		int32 RequestedSquadSize,
		const FVector& Origin,
		const FVector& Forward);

	/** Убирает всех Unit и Squad. */
	void ClearArmy();

	/**
	 * Инструмент замеров P2, не игровая механика.
	 * Соседние ряды отрядов постоянно ходят навстречу друг другу и
	 * разворачиваются по прибытии, поэтому двигаются все Unit.
	 * Приказы идут через обычную очередь команд. Включается консольной
	 * командой algona.P2.StressMove, которая есть только в не-shipping сборках.
	 */
	void SetStressMoveEnabled(bool bEnabled);

	/**
	 * То же, но все Squad идут в одну сторону и возвращаются обратно, не
	 * сталкиваясь: замер марша больших армий без столкновений.
	 */
	void SetStressMarchEnabled(bool bEnabled);

private:
	// Накопленная статистика шагов за одно окно реального времени.
	// Используется только для периодического отчёта в лог.
	struct FAlgonaMetricsReportWindow
	{
		double StartWallSeconds = 0.0;
		uint64 StartOverloadedFrameCount = 0;
		int32 StepCount = 0;
		int32 MaxStepsPerFrame = 0;

		// Кадры игры за окно: средний FPS и худший кадр. По экранному
		// счётчику судить трудно — он скачет несколько раз в секунду.
		int32 FrameCount = 0;
		double MaxFrameMilliseconds = 0.0;
		double StepMillisecondsSum = 0.0;
		double StepMillisecondsMax = 0.0;
		double CommandsMillisecondsSum = 0.0;
		double PathsMillisecondsSum = 0.0;
		double SquadsMillisecondsSum = 0.0;
		double SteerMillisecondsSum = 0.0;
		double UnitGridMillisecondsSum = 0.0;
		double LocalGridMillisecondsSum = 0.0;
		double SeparationMillisecondsSum = 0.0;
		int64 MovedEntitiesSum = 0;
	};

	// Состояние Unit в плоских массивах (Structure of Arrays): отдельный массив
	// на каждое поле, индекс = UnitId - 1.
	// Это источник истины для движения Unit. Mass-сущность Unit хранит только
	// идентичность (UnitId и тег). У каждого поля ровно один хозяин.
	struct FAlgonaUnitStateArrays
	{
		// Авторитетное состояние, переживает тики.
		TArray<FVector> Positions;
		TArray<float> FacingYaws;
		TArray<FVector2f> Velocities;
		TArray<int32> SquadIds;
		TArray<int32> SlotIndices;

		// Размер Unit: радиус тела (L3, круги выбора, выбор мышью) и масштаб
		// меша для Presentation. Разные типы существ различаются ими.
		TArray<float> Radii;
		TArray<float> MeshScales;

		// Рабочие данные текущего тика.
		// Желаемая скорость — результат L2. На шаге инерции между ней и
		// фактической скоростью появится ограничение ускорения.
		TArray<FVector2f> DesiredVelocities;

		// Новая скорость Unit, рассчитанная в первом проходе SteerUnits.
		// Отдельный массив нужен, чтобы соседи в этом проходе читали скорость
		// на начало тика (схема Якоби).
		TArray<FVector2f> NextVelocities;

		// Теснота Unit за этот тик — доля торможения из-за соседей (0 —
		// свободно, до AvoidanceMaxBrake). Из неё считается теснота Squad.
		TArray<float> Crowding;

		// 1 — Unit заметно уклоняется и должен смотреть по ходу, а не идти боком.
		TArray<uint8> AvoidanceFacingFlags;


		// L3: скорость расталкивания за этот тик. Добавляется к движению
		// поверх инерции: толчок действует сразу, а не разгоняется.
		TArray<FVector2f> SeparationVelocities;

		// Глубина касания за этот тик (0 — свободно, 1 — Unit в одной точке).
		// Из неё считаются трение и вклад контакта в тесноту Squad.
		TArray<float> ContactDepths;

		// Уклонение и торможение прошлого тика: новое решение смешивается
		// с ними, чтобы реакция на соседей не дёргалась от тика к тику.
		TArray<FVector2f> DodgeVelocities;
		TArray<float> AvoidanceBrakes;

		// 1 — позиция или поворот изменились в этом тике.
		TArray<uint8> ChangedFlags;

		// 1 — Unit перешёл в другую ячейку Unit Grid.
		TArray<uint8> CellChangedFlags;
	};

	// Данные Squad, общие для всех его Unit в текущем тике.
	struct FAlgonaSquadMovementFrame
	{
		FVector Forward = FVector::ForwardVector;
		FVector Right = FVector::RightVector;
		FVector2f CenterVelocity = FVector2f::ZeroVector;
		float YawRate = 0.0f;
		float FacingYaw = 0.0f;
		float UnitAcceleration = 0.0f;
		bool bLocalAvoidance = false;
		bool bUnitsFaceMovement = false;
		float UnitMaxSpeed = 0.0f;
	};

	bool CreateUnits(
		int32 UnitCount,
		int32 RequestedSquadSize,
		const FVector& Origin,
		const FVector& Forward,
		FAlgonaSpawnArmyResult& OutResult);
	bool CreateSquads(
		int32 RequestedSquadSize,
		int32 FirstUnitIndex,
		const FVector& Origin,
		const FVector& Forward,
		FAlgonaSpawnArmyResult& OutResult);
	void DestroyUnits();

	// Приводит массивы состояния Unit к размеру TotalUnitCount: добавленные
	// элементы получают значения по умолчанию, лишние отбрасываются.
	// Уже существующие Unit не трогает: их индексы (UnitId - 1) не меняются.
	void ResizeUnitState(int32 TotalUnitCount);

	/**
	 * Площадка Squad свободна: прямоугольник HalfExtent вокруг Center
	 * (повёрнутый по Forward) целиком лежит на проходимой земле и в нём
	 * нет Unit. Проверка проходимости пропускается, если навигации в мире
	 * нет. Реализация — AlgonaSimulationPath.cpp.
	 */
	bool IsSquadPlacementFree(
		const FVector& Center,
		const FVector& Forward,
		const FVector2D& HalfExtent,
		TArray<uint32>& ScratchUnitIds);

	// Добавляет снимок одного Unit, прочитанный из массивов состояния.
	void AppendUnitSnapshot(
		uint32 UnitId,
		TArray<FAlgonaUnitSnapshot>& OutSnapshots) const;

	/**
	 * Проверяет связь Squad <-> Unit: число активных Unit равно числу слотов,
	 * у каждого Unit правильные SquadId и SlotIndex, каждый Unit ровно в
	 * одном слоте. Вызывается после создания армии в не-shipping сборках.
	 */
	bool ValidateSquadMembership();

	void RunSimulationStep(float DeltaTime);
	void ProcessPendingCommands();
	void ApplyMoveCommand(
		FAlgonaSquad& Squad,
		const FVector& TargetLocation,
		const FVector& FinalDirection,
		int32 RowLength);
	void ApplyMoveGroupCommand(const FAlgonaSquadCommand& Command);

	// Режим движения по длине пути (шаг вбок / лицом вперёд / марш).
	static EAlgonaSquadMoveMode ChooseSquadMoveMode(
		const FAlgonaSquad& Squad,
		double PathLength);

	// --- Путь центра Squad по навигации (AlgonaSimulationPath.cpp) ---

	// Ставит Squad в очередь запросов пути. До ответа Squad идёт по прямой.
	void RequestSquadPath(FAlgonaSquad& Squad);

	// Стадия шага: считает пути для очереди запросов, не больше
	// algona.P2.PathQueriesPerTick за тик.
	void ProcessSquadPathRequests();

	// Один запрос к навигации UE. false — пути нет (или навигации нет),
	// Squad продолжает идти по прямой.
	bool BuildSquadPath(FAlgonaSquad& Squad);

	// Один запрос пути к указанной карте проходимости. Точки возвращаются
	// без первой (это сама позиция центра Squad). bOutPartial — путь обрывается
	// раньше цели: по широкой карте это значит, что строй там не проходит.
	// Приказ не выполняется: цели нет на карте проходимости или пути к ней
	// нет. Squad остаётся на месте — выбирать другую цель за игрока нельзя.
	void CancelSquadMoveOrder(FAlgonaSquad& Squad, const TCHAR* Reason);

	bool QuerySquadPathPoints(
		const FAlgonaSquad& Squad,
		const class ANavigationData& NavData,
		const FVector& StartLocation,
		double Clearance,
		TArray<FVector>& OutPoints,
		TArray<struct FAlgonaPathPortal>& OutPortals,
		FVector& OutStartLocation,
		bool& bOutPartial);

	// Приводит приказ к посчитанному пути: режим движения по длине пути и
	// конечное направление по его последнему отрезку.
	void ApplySquadPathToOrder(FAlgonaSquad& Squad);

	/**
	 * Расстояние до точки погони, см: радиус дуги, по которой строй вообще
	 * способен повернуть (скорость, делённая на угловую скорость), в
	 * пределах MinPathLookaheadCm..MaxPathLookaheadCm. Им же определяется
	 * запас, который путь держит от препятствий на повороте.
	 */
	static double GetSquadPathLookahead(const FAlgonaSquad& Squad);

	// Значения CVar algona.P2.Path*.
	static bool IsSquadPathEnabled();
	static int32 GetPathQueriesPerTick();
	static float GetPathAgentRadius();
	static float GetPathWideDetour();
	static float GetPathLookaheadOverride();

	// Переназначение Unit по слотам после Reform: каждый занимает ближайший
	// подходящий слот, чтобы никто не бежал через весь строй.
	void ReassignSquadSlotsByPosition(FAlgonaSquad& Squad);

	// Разворот больше 90°: Unit переходят в зеркальные слоты, направление
	// Squad меняется на противоположное. false — если таблица неприменима.
	bool ApplyMirrorTurn(FAlgonaSquad& Squad);

	// L3: перестройка мелкой сетки по позициям Unit после L2.
	void BuildLocalAvoidanceGrid(bool bParallel);


	// L3: расталкивание реально перекрывшихся Unit (контакт).
	void SeparateUnits(float DeltaTime, bool bParallel);

	// Сводка по Squad после движения: теснота и фактические границы.
	void UpdateSquadSummary();

	// Отбор участников L3: Squad, чьи границы пересекаются с другим Squad,
	// плюс те, кто перестраивается или поворачивается.
	void SelectLocalAvoidanceUnits();

	// Значение CVar algona.P2.SquadCongestionSlowdown.
	static float GetSquadCongestionSlowdown();

	// Значение CVar algona.P2.SquadMoveSpeed; 0 — подмены нет.
	static float GetSquadMoveSpeedOverride();

	// Границы режимов движения Squad, см (CVar algona.P2.*Distance).
	static float GetSidestepMaxDistance();
	static float GetFaceMovementMaxDistance();
	bool UpdateSquadCenters(float DeltaTime);

	// Движение Unit (AlgonaSimulationMovement.cpp).
	// bParallel — считать на рабочих потоках; результат одинаков.
	bool IsParallelMovementEnabled() const;
	void SteerUnits(float DeltaTime, bool bParallel);
	// Обновляет Unit Grid для Unit, сменивших ячейку. Возвращает число Unit,
	// у которых в этом тике изменились позиция или поворот.
	int32 UpdateUnitGrid();

	FVector ComputeSlotWorldPosition(
		const FAlgonaSquad& Squad,
		int32 SlotIndex) const;

	bool IsAuthoritativeSimulationWorld() const;

	void SubmitStressMoveCommands();

	void AccumulateMetricsReportStep();
	void UpdateMetricsReport(int32 ExecutedStepsThisFrame, float FrameDeltaTime);

	FAlgonaFixedStepAccumulator FixedStepAccumulator{
		AlgonaSimulationDefaults::FixedStepSeconds,
		AlgonaSimulationDefaults::MaxStepsPerFrame};

	uint64 SimulationTick = 0;
	uint64 StateRevision = 0;
	FAlgonaSimulationMetrics Metrics;

	UPROPERTY(Transient)
	TObjectPtr<UMassEntitySubsystem> MassEntitySubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMassSpawnerSubsystem> MassSpawnerSubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMassEntityConfigAsset> UnitEntityConfig = nullptr;

	// Active entity-level spatial index used by Presentation and future
	// simulation systems that need individual combat-unit queries.
	FAlgonaUnitSpatialGrid UnitSpatialGrid{
		AlgonaSimulationDefaults::SpatialGridCellSizeCm};

	TArray<FMassEntityHandle> UnitEntities;
	TArray<FAlgonaSquad> Squads;
	// Очередь команд до начала следующего fixed step.
	TArray<FAlgonaSquadCommand> PendingCommands;

	// Squad, ждущие расчёта пути, в порядке поступления приказов.
	TArray<int32> PathRequestQueue;

	// Про отсутствие навигации в мире и про первый неудавшийся запрос пути
	// сообщается по одному разу за запуск.
	bool bMissingNavigationLogged = false;
	bool bPathQueryFailedLogged = false;
	bool bMissingSquadNavDataLogged = false;

	// Ворота коридора последнего построенного пути, парами точек.
	// Заполняются только при algona.P2.DebugPath 2.
	TArray<FVector> DebugPathPortals;
	bool bPlacementWithoutNavigationLogged = false;

	FAlgonaMetricsReportWindow MetricsReportWindow;

	// Состояние Unit (источник истины для движения) и данные Squad текущего тика.
	FAlgonaUnitStateArrays UnitState;
	TArray<FAlgonaSquadMovementFrame> SquadMovementFrames;

	// L3: мелкая сетка соседей и список Unit, которые в неё попадают —
	// только Unit тех Squad, которым L3 нужен (SelectLocalAvoidanceUnits).
	FAlgonaTorusGrid LocalAvoidanceGrid;
	TArray<int32> LocalAvoidanceUnitIndices;

	// Отбор: крупная сетка Squad по их фактическим границам, рабочие массивы
	// границ и признак «этому Squad нужен L3» по номеру Squad.
	FAlgonaTorusGrid SquadBroadphaseGrid;
	TArray<int32> SquadBroadphaseIndices;
	TArray<FVector2f> SquadBoundsMin;
	TArray<FVector2f> SquadBoundsMax;
	TArray<uint8> SquadNeedsLocalAvoidance;
	TArray<uint8> SquadIsMoving;

	// Состояние стресс-сценария: направление следующего прохода по Y
	// (+1 или -1) для каждого SquadId.
	bool bStressMoveEnabled = false;

	// Марш: все Squad идут в одну сторону, столкновений нет.
	bool bStressMarchEnabled = false;
	TArray<int8> StressMoveDirections;
};
