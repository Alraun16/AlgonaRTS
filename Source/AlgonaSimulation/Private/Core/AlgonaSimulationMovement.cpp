#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitSteering.h"

#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

/*
 * Движение Unit за один fixed step.
 *
 * Состояние Unit хранится в плоских массивах UnitState (индекс = UnitId - 1),
 * и это источник истины: копирования из Mass и обратно нет.
 *   1. SelectLocalAvoidanceUnits — кому из Unit нужен L3 в этом тике;
 *   2. BuildLocalAvoidanceGrid   — сетка соседей по их позициям;
 *   3. SteerUnits                — намерение Unit (L2 к слоту + обход
 *                                  соседей), инерция, новая позиция, поворот;
 *   4. SeparateUnits             — контакт: расталкивание перекрывшихся;
 *   5. UpdateSquadSummary        — теснота и фактические границы Squad;
 *   6. UpdateUnitGrid            — Unit Grid для Unit, сменивших ячейку.
 * Все Unit обрабатываются каждый тик: Unit всегда потенциально подвижен.
 *
 * Многопоточность: расчёт одного Unit пишет только в элементы этого Unit,
 * а чужие данные читает из состояния, которое в этом проходе не меняется
 * (схема Якоби). Поэтому результат одинаков при любом числе потоков.
 * Unit Grid хранит ячейки в TMap и изменяется только последовательно.
 */

namespace
{
	TAutoConsoleVariable<int32> CVarAlgonaP2ParallelMovement(
		TEXT("algona.P2.ParallelMovement"),
		1,
		TEXT("1 = unit movement uses worker threads, 0 = single thread. Results are identical."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP2LocalAvoidanceGrid(
		TEXT("algona.P2.LocalAvoidanceGrid"),
		1,
		TEXT("1 = rebuild the L3 neighbour grid every step, 0 = skip it and all L3 (for measurements)."),
		ECVF_Default);

	// --- Мягкий слот (L2) ---

	TAutoConsoleVariable<float> CVarAlgonaP2SlotDeadZone(
		TEXT("algona.P2.SlotDeadZone"),
		30.0f,
		TEXT("Distance from the slot, cm, inside which a unit does not straighten up."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2SlotReturnTime(
		TEXT("algona.P2.SlotReturnTime"),
		0.8f,
		TEXT("Slot return speed is at most offset / this time, s. Higher = gentler, longer arcs back to the slot."),
		ECVF_Default);

	// --- Обход соседей (намерение, через инерцию) ---

	TAutoConsoleVariable<int32> CVarAlgonaP2Avoidance(
		TEXT("algona.P2.Avoidance"),
		1,
		TEXT("1 = units anticipate neighbours: sidestep oncoming, follow, yield; 0 = off."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2AvoidanceHorizon(
		TEXT("algona.P2.AvoidanceHorizon"),
		0.8f,
		TEXT("How far ahead in time a unit reacts to a coming collision, s."),
		ECVF_Default);

	// Щель между соседями в строю — 150 - 70 = 80 см. Требуемый зазор
	// 70 + 20 = 90 см чуть больше щели: встречные ряды проходят, но
	// заметно протискиваются и расступаются, а не проскакивают насквозь.
	TAutoConsoleVariable<float> CVarAlgonaP2AvoidancePersonalSpace(
		TEXT("algona.P2.AvoidancePersonalSpace"),
		20.0f,
		TEXT("Gap a unit tries to keep when passing another, cm."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2AvoidanceSideRatio(
		TEXT("algona.P2.AvoidanceSideRatio"),
		0.6f,
		TEXT("Max sidestep speed as a share of the unit max speed."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2AvoidancePassBrake(
		TEXT("algona.P2.AvoidancePassBrake"),
		0.45f,
		TEXT("How much speed a unit loses while passing an oncoming or standing unit (share, at the moment of contact)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2AvoidanceMaxBrake(
		TEXT("algona.P2.AvoidanceMaxBrake"),
		0.6f,
		TEXT("Max share of speed a unit loses when crowded (0.6 = down to 40%)."),
		ECVF_Default);

	// --- Контакт (поверх инерции) ---

	TAutoConsoleVariable<int32> CVarAlgonaP2Separation(
		TEXT("algona.P2.Separation"),
		1,
		TEXT("1 = overlapping units push apart, 0 = off (for comparison)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2SeparationStiffness(
		TEXT("algona.P2.SeparationStiffness"),
		8.0f,
		TEXT("Push speed per cm of overlap beyond the tolerance, 1/s."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2SeparationTolerance(
		TEXT("algona.P2.SeparationTolerance"),
		10.0f,
		TEXT("Overlap that is allowed without pushing (shoulder to shoulder), cm."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2SeparationFriction(
		TEXT("algona.P2.SeparationFriction"),
		0.6f,
		TEXT("Max share of speed a unit loses while squeezing through contact (at full overlap)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2SeparationMaxSpeed(
		TEXT("algona.P2.SeparationMaxSpeed"),
		300.0f,
		TEXT("Max push speed of one unit, cm/s."),
		ECVF_Default);

	// Потолок скорости Unit для подбора на ходу, см/с. 0 — потолок берётся
	// из Squad (CenterMoveSpeed * UnitSpeedFactor).
	TAutoConsoleVariable<float> CVarAlgonaP2UnitMaxSpeed(
		TEXT("algona.P2.UnitMaxSpeed"),
		0.0f,
		TEXT("Overrides the max unit speed in cm/s. 0 keeps the squad value."),
		ECVF_Default);

	// --- Теснота Squad (L1) ---

	TAutoConsoleVariable<float> CVarAlgonaP2SquadCongestionSlowdown(
		TEXT("algona.P2.SquadCongestionSlowdown"),
		0.4f,
		TEXT("Max share of speed a squad loses when its units are crowded (0.4 = down to 60%)."),
		ECVF_Default);

	// Ниже этой скорости Unit считается стоящим: он только расступается перед
	// идущими и при расталкивании уступает меньше движущегося, см/с.
	constexpr float StandingSpeed = 30.0f;

	// Подвижность при расхождении: чем она выше, тем большую долю сдвига
	// Unit берёт на себя. Стоящий держит место надёжнее движущегося, а
	// крупный — надёжнее мелкого: подвижность падает линейно с радиусом,
	// поэтому тролль сдвигает пехотинца вдвое охотнее, чем наоборот.
	constexpr float MovingYieldWeight = 2.0f;
	constexpr float IdleYieldWeight = 1.0f;

	float GetYieldMobility(bool bMoving, float Radius)
	{
		const float MoveFactor = bMoving ? MovingYieldWeight : IdleYieldWeight;
		return Radius > UE_SMALL_NUMBER
			? MoveFactor * FAlgonaSquad::DefaultUnitRadiusCm / Radius
			: MoveFactor;
	}

	// Клетка сетки соседей — около интервала строя: в строю обычно один Unit
	// в клетке. Дальше одной клетки (3×3) Unit смотрит, только когда успеет
	// пройти это расстояние до столкновения: на полном ходу это 5×5 клеток,
	// а в давке, где все заторможены, хватает 3×3 — и кандидатов втрое меньше.
	constexpr float LocalAvoidanceCellSizeCm = 150.0f;
	constexpr int32 AvoidanceMaxRings = 2;

	// Отбор участников L3: крупная сетка Squad и запас к их границам —
	// дальность обзора обхода плюс путь Unit за тик.
	constexpr float SquadBroadphaseCellSizeCm = 2000.0f;
	constexpr float SquadBroadphaseMarginCm =
		AvoidanceMaxRings * LocalAvoidanceCellSizeCm + 50.0f;

	// Сколько тиков после Reform Squad остаётся участником L3: при
	// перестроении Unit идут через чужие слоты и могут пройти насквозь.
	constexpr int32 ReformAvoidanceHoldTicks = 40;

	// Насколько сильно Unit тормозит при разных встречах (доля скорости
	// при столкновении «прямо сейчас»; дальше по времени — слабее).
	// - Встречный и стоящий на пути: разминуться, притормозив
	//   (algona.P2.AvoidancePassBrake).
	// - Попутный впереди: притормозить и идти следом, не обходя.
	// - Пересекающий справа: уступить («помеха справа»).
	constexpr float FollowBrake = 0.8f;
	constexpr float YieldBrake = 0.8f;

	// Косинусы углов между направлениями двух Unit: меньше -0.5 — встречные
	// (расходятся больше чем на 120°), больше 0.5 — попутные (меньше 60°).
	constexpr float OncomingCosine = -0.5f;
	constexpr float SameDirectionCosine = 0.5f;

	// Сколько опасных соседей Unit держит в поперечной оси при выборе щели.
	// Больше в реальной толпе не встречается: дальние в неё не попадают.
	constexpr int32 MaxAvoidanceThreats = 12;

	// Запас при выходе из занятого отрезка, см: чтобы щель считалась
	// свободной с небольшим зазором, а не «в притык».
	constexpr float AvoidanceGapMargin = 2.0f;

	// Сторона обхода меняется, только если щель с другой стороны заметно
	// ближе: иначе Unit виляет влево-вправо, пока протискивается.
	constexpr float AvoidanceSideSwitchRatio = 0.7f;

	// Насколько далеко Unit вообще тянется к щели, в своих радиусах.
	// Если ближайшая щель дальше, Unit не дёргается: тормозит и
	// протискивается, а разводит его толчок.
	constexpr float AvoidanceMaxGapDistanceInRadii = 3.0f;

	// Доля прошлого уклонения в новом: сглаживает смену решения, чтобы Unit
	// не дёргался, когда сосед то попадает, то не попадает на его линию.
	constexpr float AvoidanceDodgeSmoothing = 0.5f;

	// Смещение набирается к моменту встречи, но не быстрее, чем за это
	// время, с: при встрече «прямо сейчас» скорость вбок не взлетает.
	constexpr float AvoidanceMinDodgeTime = 0.1f;

	// При заметном уклонении (скорость вбок выше этой, см/с) Unit
	// поворачивается по ходу, а не идёт боком. Выключается порогом ниже,
	// иначе признак мигает на границе и модель дёргается.
	constexpr float AvoidanceFacingSpeed = 60.0f;
	constexpr float AvoidanceFacingReleaseSpeed = 30.0f;

	// Теснота Squad = средняя теснота (торможение) его Unit, усиленная:
	// заторможены обычно только передние ряды.
	constexpr float SquadCongestionGain = 1.5f;


	// Минимальная порция Unit на одну задачу рабочего потока: на более мелких
	// порциях раздача задач потокам стоит дороже самого расчёта.
	constexpr int32 SteerMinBatchSize = 1024;

	// Приводит массив состояния к размеру TotalCount: добавленные элементы
	// получают значение по умолчанию, лишние отбрасываются. Уже лежащие
	// элементы не трогаются — их индекс это UnitId - 1.
	template <typename ElementType>
	void ResizeArray(
		TArray<ElementType>& Array,
		int32 TotalCount,
		const ElementType& DefaultValue)
	{
		if (Array.Num() > TotalCount)
		{
			Array.SetNum(TotalCount, EAllowShrinking::No);
			return;
		}

		Array.Reserve(TotalCount);

		while (Array.Num() < TotalCount)
		{
			Array.Add(DefaultValue);
		}
	}

	// Опасный сосед на поперечной оси Unit: отрезок [Low, High], время до
	// сближения и вес уступки соседа.
	struct FAvoidanceThreat
	{
		float Low = 0.0f;
		float High = 0.0f;
		float Time = 0.0f;
		float OtherMobility = 0.0f;
	};

	template <typename BodyType>
	void RunForUnits(int32 UnitCount, bool bParallel, const TCHAR* DebugName, BodyType&& Body)
	{
		if (bParallel)
		{
			ParallelFor(DebugName, UnitCount, SteerMinBatchSize, Body);
		}
		else
		{
			for (int32 UnitIndex = 0; UnitIndex < UnitCount; ++UnitIndex)
			{
				Body(UnitIndex);
			}
		}
	}
}

bool UAlgonaSimulationSubsystem::IsParallelMovementEnabled() const
{
	return CVarAlgonaP2ParallelMovement.GetValueOnGameThread() != 0;
}

float UAlgonaSimulationSubsystem::GetSquadCongestionSlowdown()
{
	return CVarAlgonaP2SquadCongestionSlowdown.GetValueOnGameThread();
}

void UAlgonaSimulationSubsystem::ResizeUnitState(int32 TotalUnitCount)
{
	FAlgonaUnitStateArrays& State = UnitState;

	ResizeArray(State.Positions, TotalUnitCount, FVector::ZeroVector);
	ResizeArray(State.FacingYaws, TotalUnitCount, 0.0f);
	ResizeArray(State.Velocities, TotalUnitCount, FVector2f::ZeroVector);
	ResizeArray(State.SquadIds, TotalUnitCount, static_cast<int32>(INDEX_NONE));
	ResizeArray(State.SlotIndices, TotalUnitCount, static_cast<int32>(INDEX_NONE));
	ResizeArray(State.Radii, TotalUnitCount, FAlgonaSquad::DefaultUnitRadiusCm);
	ResizeArray(State.MeshScales, TotalUnitCount, 1.0f);
	ResizeArray(State.DesiredVelocities, TotalUnitCount, FVector2f::ZeroVector);
	ResizeArray(State.NextVelocities, TotalUnitCount, FVector2f::ZeroVector);
	ResizeArray(State.Crowding, TotalUnitCount, 0.0f);
	ResizeArray(State.AvoidanceFacingFlags, TotalUnitCount, static_cast<uint8>(0));
	ResizeArray(State.SeparationVelocities, TotalUnitCount, FVector2f::ZeroVector);
	ResizeArray(State.ContactDepths, TotalUnitCount, 0.0f);
	ResizeArray(State.DodgeVelocities, TotalUnitCount, FVector2f::ZeroVector);
	ResizeArray(State.AvoidanceBrakes, TotalUnitCount, 0.0f);
	ResizeArray(State.ChangedFlags, TotalUnitCount, static_cast<uint8>(0));
	ResizeArray(State.CellChangedFlags, TotalUnitCount, static_cast<uint8>(0));
}

void UAlgonaSimulationSubsystem::SteerUnits(
	float DeltaTime,
	bool bParallel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_SteerUnits);

	if (DeltaTime <= 0.0f)
	{
		return;
	}

	const float UnitMaxSpeedOverride = CVarAlgonaP2UnitMaxSpeed.GetValueOnGameThread();

	// Подготовка Squad: направления, скорость центра и максимальная скорость
	// Unit считаются один раз на Squad, а не для каждого Unit.
	SquadMovementFrames.SetNum(Squads.Num(), EAllowShrinking::No);

	for (int32 SquadIndex = 0; SquadIndex < Squads.Num(); ++SquadIndex)
	{
		const FAlgonaSquad& Squad = Squads[SquadIndex];
		FAlgonaSquadMovementFrame& Frame = SquadMovementFrames[SquadIndex];

		Frame.Forward = Squad.GetForwardDirection2D();
		Frame.Right = FVector::CrossProduct(
			FVector::UpVector,
			Frame.Forward).GetSafeNormal();
		Frame.FacingYaw = static_cast<float>(
			FMath::Atan2(Frame.Forward.Y, Frame.Forward.X));
		Frame.CenterVelocity = FVector2f(
			static_cast<float>(Squad.CenterVelocity.X),
			static_cast<float>(Squad.CenterVelocity.Y));
		Frame.YawRate = Squad.YawRate;
		Frame.bLocalAvoidance = SquadNeedsLocalAvoidance.IsValidIndex(SquadIndex)
			&& SquadNeedsLocalAvoidance[SquadIndex] != 0;

		// В среднем режиме Unit смотрят по направлению движения, даже стоя
		// в своих слотах: «лунный бег» боком на десяток метров выглядит странно.
		Frame.bUnitsFaceMovement =
			Squad.bHasMoveTarget
			&& Squad.MoveMode != EAlgonaSquadMoveMode::Sidestep;
		Frame.UnitAcceleration =
			Squad.CenterMoveSpeed * Squad.UnitSpeedFactor
			/ FAlgonaSquad::UnitAccelerationSeconds;
		Frame.UnitMaxSpeed = UnitMaxSpeedOverride > 0.0f
			? UnitMaxSpeedOverride
			: Squad.CenterMoveSpeed
				* (Squad.YawRate != 0.0f
					? Squad.TurningUnitSpeedFactor
					: Squad.UnitSpeedFactor);
	}

	FAlgonaUnitStateArrays& State = UnitState;
	const int32 UnitCount = State.Positions.Num();
	const int32 SquadCount = SquadMovementFrames.Num();

	// Скорость поворота тела обратно пропорциональна радиусу: крупному
	// существу развернуться тяжелее, чем пехотинцу.
	const float TurnStepPerRadius =
		FMath::DegreesToRadians(AlgonaUnitSteering::MaxTurnRateDegrees)
		* FAlgonaSquad::DefaultUnitRadiusCm
		* DeltaTime;
	const float FaceMovementMinDistanceSquared =
		FMath::Square(AlgonaUnitSteering::FaceMovementMinDistance);
	const float IdleSpeedSquared =
		FMath::Square(AlgonaUnitSteering::IdleSpeed);
	const float StandingSpeedSquared = FMath::Square(StandingSpeed);

	const float SlotDeadZone = CVarAlgonaP2SlotDeadZone.GetValueOnGameThread();
	const float SlotReturnTime = CVarAlgonaP2SlotReturnTime.GetValueOnGameThread();

	// Чередование половин Unit по номеру: в чётный тик пересчитывают одни,
	// в нечётный другие.
	const int32 AvoidanceTickPhase = static_cast<int32>(SimulationTick & 1);

	const bool bAvoidance =
		CVarAlgonaP2Avoidance.GetValueOnGameThread() != 0
		&& CVarAlgonaP2LocalAvoidanceGrid.GetValueOnGameThread() != 0
		&& LocalAvoidanceGrid.GetEntryCount() > 0;
	const float AvoidanceHorizon =
		FMath::Max(CVarAlgonaP2AvoidanceHorizon.GetValueOnGameThread(), UE_KINDA_SMALL_NUMBER);
	const float PersonalSpace = CVarAlgonaP2AvoidancePersonalSpace.GetValueOnGameThread();
	const float SideRatio = CVarAlgonaP2AvoidanceSideRatio.GetValueOnGameThread();
	const float PassBrake = CVarAlgonaP2AvoidancePassBrake.GetValueOnGameThread();
	const float ContactFriction = FMath::Clamp(
		CVarAlgonaP2SeparationFriction.GetValueOnGameThread(), 0.0f, 1.0f);
	const float* ContactDepthData = State.ContactDepths.GetData();
	FVector2f* DodgeData = State.DodgeVelocities.GetData();
	float* BrakeData = State.AvoidanceBrakes.GetData();
	const float MaxBrake = FMath::Clamp(CVarAlgonaP2AvoidanceMaxBrake.GetValueOnGameThread(), 0.0f, 1.0f);

	// Указатели вместо TArray[] в горячих циклах (см. раздел 4f ADR).
	const FAlgonaSquadMovementFrame* FrameData = SquadMovementFrames.GetData();
	const int32* SquadIdData = State.SquadIds.GetData();
	const int32* SlotIndexData = State.SlotIndices.GetData();
	const float* RadiusData = State.Radii.GetData();
	FVector* PositionData = State.Positions.GetData();
	FVector2f* VelocityData = State.Velocities.GetData();
	FVector2f* DesiredData = State.DesiredVelocities.GetData();
	FVector2f* NextVelocityData = State.NextVelocities.GetData();
	float* CrowdingData = State.Crowding.GetData();
	uint8* AvoidanceFacingData = State.AvoidanceFacingFlags.GetData();
	float* FacingYawData = State.FacingYaws.GetData();
	uint8* ChangedData = State.ChangedFlags.GetData();
	uint8* CellChangedData = State.CellChangedFlags.GetData();

	// Положение слота Unit в мире по данным Squad на этот тик.
	auto GetSlotPosition = [this, FrameData](int32 SquadId, int32 SlotIndex, FVector2f& OutLocalOffset)
	{
		const FAlgonaSquad& Squad = Squads[SquadId];
		const FAlgonaSquadMovementFrame& Frame = FrameData[SquadId];
		OutLocalOffset = Squad.FormationLayout.Slots[SlotIndex].LocalOffset;

		return Squad.CenterLocation
			+ Frame.Forward * static_cast<double>(OutLocalOffset.X)
			+ Frame.Right * static_cast<double>(OutLocalOffset.Y);
	};

	auto HasSlot = [this, SquadCount](int32 SquadId, int32 SlotIndex)
	{
		return SquadId >= 0
			&& SquadId < SquadCount
			&& Squads[SquadId].FormationLayout.Slots.IsValidIndex(SlotIndex);
	};

	// 1. Намерение и инерция. Читает позиции и скорости всех Unit на начало
	// тика, пишет только новую скорость своего Unit (схема Якоби).
	auto ComputeVelocity = [&](int32 UnitIndex)
	{
		const int32 SquadId = SquadIdData[UnitIndex];
		const int32 SlotIndex = SlotIndexData[UnitIndex];

		// Unit без Squad или без слота стоит на месте.
		if (!HasSlot(SquadId, SlotIndex))
		{
			DesiredData[UnitIndex] = FVector2f::ZeroVector;
			NextVelocityData[UnitIndex] = FVector2f::ZeroVector;
			CrowdingData[UnitIndex] = 0.0f;
			AvoidanceFacingData[UnitIndex] = 0;
			DodgeData[UnitIndex] = FVector2f::ZeroVector;
			BrakeData[UnitIndex] = 0.0f;
			return;
		}

		const FAlgonaSquadMovementFrame& Frame = FrameData[SquadId];
		FVector2f LocalOffset;
		const FVector SlotPosition = GetSlotPosition(SquadId, SlotIndex, LocalOffset);
		const FVector& Position = PositionData[UnitIndex];

		// Скорость слота = скорость центра + вклад поворота строя.
		// При повороте на YawRate вектор «вперёд» движется вдоль «вправо»,
		// а «вправо» — против «вперёд», поэтому вклад поворота равен
		// YawRate * (X * Right - Y * Forward). Дальние слоты быстрее.
		const FVector2f SlotVelocity(
			Frame.CenterVelocity.X + Frame.YawRate * static_cast<float>(
				LocalOffset.X * Frame.Right.X - LocalOffset.Y * Frame.Forward.X),
			Frame.CenterVelocity.Y + Frame.YawRate * static_cast<float>(
				LocalOffset.X * Frame.Right.Y - LocalOffset.Y * Frame.Forward.Y));

		// L2: желаемая скорость к мягкому слоту. Squad в этом тике уже сдвинут
		// и повёрнут, поэтому отставание считается от слота в начале тика.
		const FVector2f ToSlotAtTickStart(
			static_cast<float>(SlotPosition.X - Position.X)
				- SlotVelocity.X * DeltaTime,
			static_cast<float>(SlotPosition.Y - Position.Y)
				- SlotVelocity.Y * DeltaTime);

		FVector2f Desired = AlgonaUnitSteering::ComputeDesiredVelocity(
			ToSlotAtTickStart,
			SlotVelocity,
			Frame.UnitMaxSpeed,
			DeltaTime,
			SlotDeadZone,
			SlotReturnTime);

		// Трение касания с прошлого тика ограничивает намерение, а не
		// фактическую скорость: иначе Unit каждый тик тормозится и тут же
		// разгоняется инерцией, и это видно как дрожание.
		const float ContactBrake = ContactDepthData[UnitIndex] * ContactFriction;
		if (ContactBrake > 0.0f)
		{
			Desired *= 1.0f - ContactBrake;
		}

		float Crowding = ContactBrake;
		bool bAvoidanceFacing = false;
		const FVector2f Velocity = VelocityData[UnitIndex];

		// Обход соседей: Unit заранее меняет курс и скорость по тем, с кем
		// столкнётся в ближайшие AvoidanceHorizon секунд. Результат идёт
		// в намерение — через инерцию, поэтому получается дуга, а не отскок.
		// Уклоняются обе стороны: идущий и стоящий, на которого надвигаются.
		if (bAvoidance && Frame.bLocalAvoidance)
		{
			// Решение об обходе пересчитывается через тик: горизонт реакции
			// 0.8 с, и задержка в 25 мс на нём незаметна, а работы вдвое
			// меньше. Половины Unit чередуются по номеру, поэтому нагрузка
			// на тик ровная. В промежуточный тик действует прошлое решение —
			// оно и так сглаживается между тиками.
			if (((UnitIndex + AvoidanceTickPhase) & 1) == 0)
			{
			// Скорость намерения, а не фактическая: иначе собственное
			// торможение уменьшает угрозу, угроза отпускает тормоз, и Unit
			// дрожит в такт тикам.
			const FVector2f IntentVelocity = Desired;
			const bool bMoving = IntentVelocity.SizeSquared() > StandingSpeedSquared;
			const FVector2f MoveDirection = bMoving
				? IntentVelocity.GetSafeNormal()
				: FVector2f::ZeroVector;
			const FVector2f RightDirection(-MoveDirection.Y, MoveDirection.X);
			const float MyMobility = GetYieldMobility(bMoving, RadiusData[UnitIndex]);

			FVector2f Dodge = FVector2f::ZeroVector;
			float Brake = 0.0f;

			// Опасные соседи на поперечной оси: отрезок, который каждый займёт
			// к моменту наибольшего сближения.
			FAvoidanceThreat Threats[MaxAvoidanceThreats];
			int32 ThreatCount = 0;

			// Дальность обзора по скорости: сколько Unit пройдёт за горизонт
			// реакции. Стоящий смотрит на 3×3 — до него самого доберутся
			// идущие, которые уже уклоняются.
			const float LookaheadDistance = IntentVelocity.Size() * AvoidanceHorizon;
			const int32 QueryRings = FMath::Clamp(
				FMath::CeilToInt32(LookaheadDistance / LocalAvoidanceCellSizeCm),
				1,
				AvoidanceMaxRings);

			const float AvoidanceQueryRadius = QueryRings * LocalAvoidanceCellSizeCm;

			LocalAvoidanceGrid.ForEachCandidate(Position, QueryRings, [&](int32 OtherIndex)
			{
				if (OtherIndex == UnitIndex)
				{
					return;
				}

				const int32 OtherSquadId = SquadIdData[OtherIndex];
				if (OtherSquadId < 0 || OtherSquadId >= SquadCount)
				{
					return;
				}

				const FVector& OtherPosition = PositionData[OtherIndex];
				const FVector2f ToOther(
					static_cast<float>(OtherPosition.X - Position.X),
					static_cast<float>(OtherPosition.Y - Position.Y));

				if (ToOther.SizeSquared() > FMath::Square(AvoidanceQueryRadius))
				{
					return;
				}

				// Идущий смотрит только вперёд: о тех, кто позади, заботятся они.
				if (bMoving && FVector2f::DotProduct(ToOther, MoveDirection) <= 0.0f)
				{
					return;
				}

				const FVector2f OtherVelocity = VelocityData[OtherIndex];
				const bool bOtherMoving = OtherVelocity.SizeSquared() > StandingSpeedSquared;

				// Два стоящих друг другу не мешают.
				if (!bMoving && !bOtherMoving)
				{
					return;
				}

				const FVector2f ClosingVelocity = IntentVelocity - OtherVelocity;
				const float CollisionDistance =
					RadiusData[UnitIndex] + RadiusData[OtherIndex] + PersonalSpace;

				const float TimeToCollision = AlgonaUnitSteering::ComputeTimeToCollision(
					ToOther,
					ClosingVelocity,
					CollisionDistance);

				// Столкновение в пределах обзора по времени: от этого зависит
				// торможение. Отрезок на поперечной оси откладывается и без
				// него — по всем соседям впереди, иначе Unit целится в щель,
				// которой в ряду на самом деле нет, и виляет от тика к тику.
				const bool bCollisionAhead =
					TimeToCollision >= 0.0f && TimeToCollision <= AvoidanceHorizon;

				// Чем ближе столкновение по времени, тем сильнее торможение.
				const float Urgency = bCollisionAhead
					? 1.0f - TimeToCollision / AvoidanceHorizon
					: 0.0f;

				if (bMoving && bOtherMoving)
				{
					const float Cosine = FVector2f::DotProduct(
						MoveDirection,
						OtherVelocity.GetSafeNormal());

					if (Cosine > SameDirectionCosine)
					{
						// Попутный впереди: притормозить и идти следом.
						// В стену он не превращается: идёт туда же, куда и я.
						Brake = FMath::Max(Brake, Urgency * FollowBrake);
						return;
					}

					if (Cosine >= OncomingCosine)
					{
						// Пересекающий: уступает тот, у кого сосед справа;
						// тот, у кого сосед слева, идёт дальше.
						if (FVector2f::DotProduct(ToOther, RightDirection) > 0.0f)
						{
							Brake = FMath::Max(Brake, Urgency * YieldBrake);
						}
						return;
					}
				}

				// Уже соприкасаются: разводит толчок (SeparateUnits).
				// Уклонение здесь только добавляло бы рывки.
				if (ToOther.Size() < CollisionDistance)
				{
					return;
				}

				// Встречный, стоящий на пути или (для стоящего) надвигающийся:
				// разминуться. Сосед откладывается на поперечную ось — линию
				// поперёк моего хода — как отрезок, который он займёт к моменту
				// наибольшего сближения. Сам я стою в нуле этой оси.
				if (ThreatCount < MaxAvoidanceThreats)
				{
					const float ClosingSpeedSquared = ClosingVelocity.SizeSquared();
					const float ClosestTime = ClosingSpeedSquared > UE_SMALL_NUMBER
						? FVector2f::DotProduct(ToOther, ClosingVelocity) / ClosingSpeedSquared
						: 0.0f;

					const float LateralCenter = FVector2f::DotProduct(
						ToOther - ClosingVelocity * ClosestTime,
						RightDirection);

					FAvoidanceThreat& Threat = Threats[ThreatCount++];
					Threat.Low = LateralCenter - CollisionDistance;
					Threat.High = LateralCenter + CollisionDistance;
					Threat.Time = bCollisionAhead ? TimeToCollision : -1.0f;
					Threat.OtherMobility = GetYieldMobility(bOtherMoving, RadiusData[OtherIndex]);
				}

				if (bMoving && bCollisionAhead)
				{
					Brake = FMath::Max(Brake, Urgency * PassBrake);
				}
			});

			// Выбор щели: ближайший сдвиг вбок, при котором я никого не задену.
			// Стороны получаются разными сами собой — кто чуть левее встречного,
			// уходит влево, кто правее — вправо, поэтому строй не сносит
			// в одну сторону. Правило «все вправо» здесь больше не нужно.
			if (ThreatCount > 0)
			{
				// Занят ли мой путь: попадаю ли я в чей-то отрезок и дойдёт ли
				// до столкновения. Щели ищутся по всем соседям впереди, а
				// уклоняться есть смысл только от того, с кем столкнусь.
				const FAvoidanceThreat* Blocker = nullptr;

				for (int32 Index = 0; Index < ThreatCount; ++Index)
				{
					const FAvoidanceThreat& Threat = Threats[Index];
					if (Threat.Time >= 0.0f
						&& Threat.Low < 0.0f
						&& Threat.High > 0.0f
						&& (Blocker == nullptr || Threat.Time < Blocker->Time))
					{
						Blocker = &Threat;
					}
				}

				if (Blocker != nullptr)
				{
					// Края занятых отрезков — кандидаты на щель. Подходит тот,
					// что ближе к нулю и не попадает в другой отрезок.
					// При равенстве сторону решает моё место в строю: левая
					// половина уходит влево, правая вправо — строй раскрывается
					// и обтекает с двух сторон, а не сносится целиком.
					// Сторона, выбранная в прошлом тике: её Unit и держится.
					const float PreviousSide = FVector2f::DotProduct(
						DodgeData[UnitIndex],
						RightDirection);

					const float TieSide = PreviousSide > UE_SMALL_NUMBER
						? 1.0f
						: (PreviousSide < -UE_SMALL_NUMBER
							? -1.0f
							: (LocalOffset.Y >= 0.0f ? 1.0f : -1.0f));

					float BestOffset = 0.0f;
					bool bFoundGap = false;

					for (int32 Index = 0; Index < ThreatCount; ++Index)
					{
						const float Candidates[] =
						{
							Threats[Index].High + AvoidanceGapMargin,
							Threats[Index].Low - AvoidanceGapMargin
						};

						for (const float Candidate : Candidates)
						{
							bool bFree = true;
							for (int32 OtherThreat = 0; OtherThreat < ThreatCount; ++OtherThreat)
							{
								if (Candidate > Threats[OtherThreat].Low
									&& Candidate < Threats[OtherThreat].High)
								{
									bFree = false;
									break;
								}
							}

							if (!bFree)
							{
								continue;
							}

							const float BestDistance = FMath::Abs(BestOffset);
							const float CandidateDistance = FMath::Abs(Candidate);

							// Своя сторона (выбранная раньше) держится, пока
							// чужая не станет заметно ближе.
							const bool bCandidateOnTieSide = Candidate * TieSide >= 0.0f;
							const bool bBestOnTieSide = BestOffset * TieSide >= 0.0f;

							const float CandidateWeight = bCandidateOnTieSide
								? CandidateDistance * AvoidanceSideSwitchRatio
								: CandidateDistance;
							const float BestWeight = bBestOnTieSide
								? BestDistance * AvoidanceSideSwitchRatio
								: BestDistance;

							const bool bBetter = !bFoundGap
								|| CandidateWeight < BestWeight - AvoidanceGapMargin
								|| (CandidateWeight < BestWeight + AvoidanceGapMargin
									&& bCandidateOnTieSide && !bBestOnTieSide);

							if (bBetter)
							{
								BestOffset = Candidate;
								bFoundGap = true;
							}
						}
					}

					// Щель дальше, чем Unit разумно тянуться: не дёргаться,
					// а протискиваться — разведёт толчок.
					if (bFoundGap
						&& FMath::Abs(BestOffset)
							> RadiusData[UnitIndex] * AvoidanceMaxGapDistanceInRadii)
					{
						bFoundGap = false;
					}

					if (bFoundGap)
					{
						// Сдвигаюсь на свою долю: идущий берёт 2/3, стоящий 1/3,
						// остальное проходит встречный.
						const float Share = MyMobility / (MyMobility + Blocker->OtherMobility);

						Dodge = RightDirection * (BestOffset * Share
							/ FMath::Max(Blocker->Time, AvoidanceMinDodgeTime));
					}
				}
			}

			Brake = FMath::Min(Brake, MaxBrake);
			Brake = FMath::Lerp(BrakeData[UnitIndex], Brake, 1.0f - AvoidanceDodgeSmoothing);
			BrakeData[UnitIndex] = Brake;

			Dodge = Dodge.GetClampedToMaxSize(SideRatio * Frame.UnitMaxSpeed);

			// Сглаживание между тиками: даже при смене стороны поворот идёт
			// плавно, а не рывком.
			Dodge = FMath::Lerp(DodgeData[UnitIndex], Dodge, 1.0f - AvoidanceDodgeSmoothing);
			DodgeData[UnitIndex] = Dodge;
			}

			// Решение этого или прошлого тика применяется одинаково.
			const float Brake = BrakeData[UnitIndex];
			const FVector2f Dodge = DodgeData[UnitIndex];

			if (Brake > 0.0f || !Dodge.IsNearlyZero())
			{
				Desired = Desired * (1.0f - Brake) + Dodge;
				Desired = Desired.GetClampedToMaxSize(Frame.UnitMaxSpeed);

				// Теснота — только торможение: обходы сами по себе не повод
				// замедлять весь строй.
				Crowding = FMath::Max(Crowding, Brake);

				const float FacingThreshold = AvoidanceFacingData[UnitIndex] != 0
					? AvoidanceFacingReleaseSpeed
					: AvoidanceFacingSpeed;
				bAvoidanceFacing = Dodge.SizeSquared() > FMath::Square(FacingThreshold);
			}
		}
		else
		{
			// Unit вне L3: прошлое решение не должно действовать дальше.
			BrakeData[UnitIndex] = 0.0f;
			DodgeData[UnitIndex] = FVector2f::ZeroVector;
		}

		DesiredData[UnitIndex] = Desired;
		CrowdingData[UnitIndex] = Crowding;
		AvoidanceFacingData[UnitIndex] = bAvoidanceFacing ? 1 : 0;

		// Инерция: фактическая скорость подтягивается к намерению не мгновенно.
		NextVelocityData[UnitIndex] = AlgonaUnitSteering::StepVelocityTowards(
			Velocity,
			Desired,
			Frame.UnitAcceleration,
			Frame.UnitAcceleration * FAlgonaSquad::DecelerationFactor,
			DeltaTime);
	};

	// 2. Движение и поворот. Пишет только свой Unit.
	auto MoveUnit = [&](int32 UnitIndex)
	{
		uint8& bChanged = ChangedData[UnitIndex];
		bChanged = 0;
		CellChangedData[UnitIndex] = 0;

		FVector2f& Velocity = VelocityData[UnitIndex];
		Velocity = NextVelocityData[UnitIndex];

		const int32 SquadId = SquadIdData[UnitIndex];
		const int32 SlotIndex = SlotIndexData[UnitIndex];

		if (!HasSlot(SquadId, SlotIndex))
		{
			return;
		}

		const FAlgonaSquadMovementFrame& Frame = FrameData[SquadId];
		FVector2f LocalOffset;
		const FVector SlotPosition = GetSlotPosition(SquadId, SlotIndex, LocalOffset);
		FVector& Position = PositionData[UnitIndex];

		// Новая позиция.
		const FVector2f MoveDelta = Velocity * DeltaTime;
		if (MoveDelta.SizeSquared() > UE_KINDA_SMALL_NUMBER)
		{
			Position.X += MoveDelta.X;
			Position.Y += MoveDelta.Y;
			bChanged = 1;
		}

		if (Position.Z != SlotPosition.Z)
		{
			Position.Z = SlotPosition.Z;
			bChanged = 1;
		}

		// Поворот: далеко от слота, в среднем режиме или при заметном
		// уклонении в движении — по ходу движения; иначе по направлению Squad.
		const float DistanceToSlotSquared = FVector2f(
			static_cast<float>(SlotPosition.X - Position.X),
			static_cast<float>(SlotPosition.Y - Position.Y)).SizeSquared();

		const bool bFaceMovement =
			(Frame.bUnitsFaceMovement
				|| DistanceToSlotSquared > FaceMovementMinDistanceSquared
				|| AvoidanceFacingData[UnitIndex] != 0)
			&& DesiredData[UnitIndex].SizeSquared() > IdleSpeedSquared;

		// Поворот по намерению, а не по фактической скорости: толчки и мелкие
		// поправки не должны крутить модель.
		const FVector2f FacingVelocity = DesiredData[UnitIndex];

		const float TargetYaw = bFaceMovement
			? static_cast<float>(FMath::Atan2(FacingVelocity.Y, FacingVelocity.X))
			: Frame.FacingYaw;

		const float Radius = FMath::Max(RadiusData[UnitIndex], UE_SMALL_NUMBER);

		float& FacingYaw = FacingYawData[UnitIndex];
		const float NewFacingYaw = AlgonaUnitSteering::StepYawTowards(
			FacingYaw,
			TargetYaw,
			TurnStepPerRadius / Radius);

		if (NewFacingYaw != FacingYaw)
		{
			FacingYaw = NewFacingYaw;
			bChanged = 1;
		}

		// Unit Grid здесь только читается: помечаются Unit, которым нужна
		// другая ячейка. Сама сетка обновляется последовательно.
		if (bChanged != 0
			&& UnitSpatialGrid.NeedsCellUpdate(
				static_cast<uint32>(UnitIndex + 1),
				Position))
		{
			CellChangedData[UnitIndex] = 1;
		}
	};

	RunForUnits(UnitCount, bParallel, TEXT("AlgonaSimulation_SteerVelocity"), ComputeVelocity);
	RunForUnits(UnitCount, bParallel, TEXT("AlgonaSimulation_MoveUnits"), MoveUnit);
}

void UAlgonaSimulationSubsystem::SelectLocalAvoidanceUnits()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_SelectLocalAvoidance);

	const int32 SquadCount = Squads.Num();
	SquadNeedsLocalAvoidance.SetNumUninitialized(SquadCount, EAllowShrinking::No);
	SquadIsMoving.SetNumUninitialized(SquadCount, EAllowShrinking::No);
	SquadBoundsMin.SetNumUninitialized(SquadCount, EAllowShrinking::No);
	SquadBoundsMax.SetNumUninitialized(SquadCount, EAllowShrinking::No);
	SquadBroadphaseIndices.Reset(SquadCount);
	LocalAvoidanceUnitIndices.Reset(UnitState.Positions.Num());

	if (CVarAlgonaP2LocalAvoidanceGrid.GetValueOnGameThread() == 0)
	{
		FMemory::Memzero(SquadNeedsLocalAvoidance.GetData(), SquadCount * sizeof(uint8));
		return;
	}

	// 1. Границы с запасом на дальность обзора обхода: Unit у края Squad
	// должен увидеть чужих заранее.
	for (int32 SquadIndex = 0; SquadIndex < SquadCount; ++SquadIndex)
	{
		FAlgonaSquad& Squad = Squads[SquadIndex];

		// Перестроение и поворот: Unit идут через чужие слоты, поэтому L3
		// нужен даже одинокому Squad.
		if (Squad.LocalAvoidanceFormationRevision != Squad.FormationRevision)
		{
			Squad.LocalAvoidanceFormationRevision = Squad.FormationRevision;
			Squad.LocalAvoidanceHoldTicks = ReformAvoidanceHoldTicks;
		}
		else if (Squad.YawRate != 0.0f)
		{
			Squad.LocalAvoidanceHoldTicks = ReformAvoidanceHoldTicks;
		}
		else if (Squad.LocalAvoidanceHoldTicks > 0)
		{
			--Squad.LocalAvoidanceHoldTicks;
		}

		SquadNeedsLocalAvoidance[SquadIndex] = Squad.LocalAvoidanceHoldTicks > 0 ? 1 : 0;

		// Стоящий Squad рядом со стоящим соседом никому не мешает: чтобы
		// пара попала в L3, двигаться должен хотя бы один из них.
		SquadIsMoving[SquadIndex] =
			(Squad.CenterSpeed > StandingSpeed
				|| Squad.YawRate != 0.0f
				|| Squad.LocalAvoidanceHoldTicks > 0)
			? 1 : 0;

		const FVector2f Margin(SquadBroadphaseMarginCm, SquadBroadphaseMarginCm);
		SquadBoundsMin[SquadIndex] = Squad.UnitBoundsMin - Margin;
		SquadBoundsMax[SquadIndex] = Squad.UnitBoundsMax + Margin;

		if (!Squad.ActiveUnitIds.IsEmpty())
		{
			SquadBroadphaseIndices.Add(SquadIndex);
		}
	}

	// 2. Крупная сетка Squad по этим границам и поиск пересечений.
	// Squad немного (десятки на 100 000 Unit — тысячи), поэтому всё
	// последовательно.
	SquadBroadphaseGrid.RebuildFromBounds(
		SquadBroadphaseCellSizeCm,
		SquadBroadphaseIndices,
		SquadBoundsMin,
		SquadBoundsMax);

	for (const int32 SquadIndex : SquadBroadphaseIndices)
	{
		if (SquadNeedsLocalAvoidance[SquadIndex] != 0)
		{
			continue;
		}

		const FVector2f& BoundsMin = SquadBoundsMin[SquadIndex];
		const FVector2f& BoundsMax = SquadBoundsMax[SquadIndex];

		SquadBroadphaseGrid.ForEachCandidateInBounds(
			BoundsMin,
			BoundsMax,
			[&](int32 OtherSquadIndex)
			{
				if (OtherSquadIndex == SquadIndex
					|| SquadNeedsLocalAvoidance[SquadIndex] != 0)
				{
					return;
				}

				if (SquadIsMoving[SquadIndex] == 0
					&& SquadIsMoving[OtherSquadIndex] == 0)
				{
					return;
				}

				if (BoundsMin.X <= SquadBoundsMax[OtherSquadIndex].X
					&& BoundsMax.X >= SquadBoundsMin[OtherSquadIndex].X
					&& BoundsMin.Y <= SquadBoundsMax[OtherSquadIndex].Y
					&& BoundsMax.Y >= SquadBoundsMin[OtherSquadIndex].Y)
				{
					SquadNeedsLocalAvoidance[SquadIndex] = 1;
				}
			});
	}

	// 3. Список Unit по возрастанию номера, как требует сетка: проход по
	// всем Unit с проверкой признака их Squad. Если L3 не нужен никому,
	// проход не делается вовсе — сто тысяч проверок впустую заметны.
	bool bAnySquadNeedsAvoidance = false;
	for (int32 SquadIndex = 0; SquadIndex < SquadCount; ++SquadIndex)
	{
		bAnySquadNeedsAvoidance |= SquadNeedsLocalAvoidance[SquadIndex] != 0;
	}

	if (!bAnySquadNeedsAvoidance)
	{
		return;
	}

	const int32* SquadIdData = UnitState.SquadIds.GetData();
	const int32 UnitCount = UnitState.Positions.Num();

	for (int32 UnitIndex = 0; UnitIndex < UnitCount; ++UnitIndex)
	{
		const int32 SquadId = SquadIdData[UnitIndex];
		if (SquadId >= 0
			&& SquadId < SquadCount
			&& SquadNeedsLocalAvoidance[SquadId] != 0)
		{
			LocalAvoidanceUnitIndices.Add(UnitIndex);
		}
	}
}

void UAlgonaSimulationSubsystem::BuildLocalAvoidanceGrid(bool bParallel)
{
	if (LocalAvoidanceUnitIndices.IsEmpty())
	{
		LocalAvoidanceGrid.Reset();
		return;
	}

	LocalAvoidanceGrid.RebuildFromPoints(
		LocalAvoidanceCellSizeCm,
		LocalAvoidanceUnitIndices,
		UnitState.Positions,
		UnitState.Radii,
		bParallel);
}

void UAlgonaSimulationSubsystem::SeparateUnits(
	float DeltaTime,
	bool bParallel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_SeparateUnits);

	if (DeltaTime <= 0.0f
		|| CVarAlgonaP2Separation.GetValueOnGameThread() == 0
		|| CVarAlgonaP2LocalAvoidanceGrid.GetValueOnGameThread() == 0
		|| LocalAvoidanceGrid.GetEntryCount() == 0)
	{
		return;
	}

	FAlgonaUnitStateArrays& State = UnitState;
	const int32 UnitCount = State.Positions.Num();
	const int32 SquadCount = SquadMovementFrames.Num();

	// Никто не участвует в L3: глубины касания обнуляются одним махом,
	// а проход по всем Unit не делается.
	if (LocalAvoidanceUnitIndices.IsEmpty())
	{
		FMemory::Memzero(State.ContactDepths.GetData(), UnitCount * sizeof(float));
		return;
	}

	const float Stiffness = CVarAlgonaP2SeparationStiffness.GetValueOnGameThread();
	const float Tolerance = FMath::Max(CVarAlgonaP2SeparationTolerance.GetValueOnGameThread(), 0.0f);
	const float MaxPushSpeed = CVarAlgonaP2SeparationMaxSpeed.GetValueOnGameThread();
	const float StandingSpeedSquared = FMath::Square(StandingSpeed);

	const FVector* PositionData = State.Positions.GetData();
	const FVector2f* VelocityData = State.Velocities.GetData();
	const int32* SquadIdData = State.SquadIds.GetData();
	const float* RadiusData = State.Radii.GetData();
	const FAlgonaSquadMovementFrame* FrameData = SquadMovementFrames.GetData();
	FVector2f* PushData = State.SeparationVelocities.GetData();
	float* CrowdingData = State.Crowding.GetData();
	float* ContactData = State.ContactDepths.GetData();

	// 1. Скорость толчка каждого Unit: только реальное перекрытие сверх
	// допуска «плечом к плечу». Обход — в намерении (SteerUnits), здесь
	// лишь страховка, чтобы Unit не проходили друг сквозь друга. Сетка
	// построена по позициям на начало тика; за тик Unit сдвигаются на
	// сантиметры, поэтому 3×3 клеток по-прежнему хватает.
	auto ComputePush = [&](int32 UnitIndex)
	{
		FVector2f Push = FVector2f::ZeroVector;
		float Contact = 0.0f;
		const int32 SquadId = SquadIdData[UnitIndex];

		if (SquadId < 0 || SquadId >= SquadCount)
		{
			PushData[UnitIndex] = Push;
			ContactData[UnitIndex] = 0.0f;
			return;
		}

		if (!FrameData[SquadId].bLocalAvoidance)
		{
			PushData[UnitIndex] = Push;
			ContactData[UnitIndex] = 0.0f;
			return;
		}

		const FVector& Position = PositionData[UnitIndex];
		const float Radius = RadiusData[UnitIndex];
		const float MyMobility = GetYieldMobility(
			VelocityData[UnitIndex].SizeSquared() > StandingSpeedSquared,
			Radius);

		LocalAvoidanceGrid.ForEachCandidate(Position, 1, [&](int32 OtherIndex)
		{
			if (OtherIndex == UnitIndex)
			{
				return;
			}

			const int32 OtherSquadId = SquadIdData[OtherIndex];
			if (OtherSquadId < 0 || OtherSquadId >= SquadCount)
			{
				return;
			}

			const FVector& OtherPosition = PositionData[OtherIndex];
			const FVector2f Offset(
				static_cast<float>(Position.X - OtherPosition.X),
				static_cast<float>(Position.Y - OtherPosition.Y));

			const float PushDistance =
				Radius + RadiusData[OtherIndex] - Tolerance;
			const float DistanceSquared = Offset.SizeSquared();

			if (PushDistance <= 0.0f || DistanceSquared >= FMath::Square(PushDistance))
			{
				return;
			}

			// Направление «от соседа ко мне». Если Unit стоят в одной точке,
			// направление выбирается по номерам — одинаково в обоих Unit пары.
			const float Distance = FMath::Sqrt(DistanceSquared);
			const FVector2f Normal = Distance > UE_KINDA_SMALL_NUMBER
				? Offset / Distance
				: FVector2f(UnitIndex < OtherIndex ? -1.0f : 1.0f, 0.0f);

			const float OtherMobility = GetYieldMobility(
				VelocityData[OtherIndex].SizeSquared() > StandingSpeedSquared,
				RadiusData[OtherIndex]);
			const float Share = MyMobility / (MyMobility + OtherMobility);

			const float Overlap = PushDistance - Distance;
			Push += Normal * (Overlap * Stiffness * Share);

			// Глубина касания: 0 — едва коснулись, 1 — стоят в одной точке.
			Contact = FMath::Max(Contact, FMath::Min(Overlap / PushDistance, 1.0f));
		});

		// Толчок за тик ограничен: в давке Unit не «выстреливают».
		PushData[UnitIndex] = Push.GetClampedToMaxSize(MaxPushSpeed);
		ContactData[UnitIndex] = Contact;

		// Касание добавляется в тесноту Squad: строй в толпе сбавляет ход.
		CrowdingData[UnitIndex] = FMath::Max(CrowdingData[UnitIndex], Contact);
	};

	// 2. Применение: толчок не проходит через инерцию, иначе Unit успевали
	// бы пройти друг сквозь друга, пока толчок набирает силу.
	uint8* ChangedData = State.ChangedFlags.GetData();
	uint8* CellChangedData = State.CellChangedFlags.GetData();
	FVector* MutablePositionData = State.Positions.GetData();

	auto ApplyPush = [&](int32 UnitIndex)
	{
		const int32 SquadId = SquadIdData[UnitIndex];
		if (SquadId < 0 || SquadId >= SquadCount)
		{
			return;
		}

		// Толчок добавляется поверх движения, но общая скорость Unit не
		// превышает его максимум: иначе после перестроения Unit «вылетали»
		// на свои слоты быстрее, чем вообще умеют бегать.
		const FVector2f Combined = (VelocityData[UnitIndex] + PushData[UnitIndex])
			.GetClampedToMaxSize(FrameData[SquadId].UnitMaxSpeed);

		const FVector2f Delta = (Combined - VelocityData[UnitIndex]) * DeltaTime;
		if (Delta.SizeSquared() <= UE_KINDA_SMALL_NUMBER)
		{
			return;
		}

		FVector& Position = MutablePositionData[UnitIndex];
		Position.X += Delta.X;
		Position.Y += Delta.Y;
		ChangedData[UnitIndex] = 1;

		if (UnitSpatialGrid.NeedsCellUpdate(
				static_cast<uint32>(UnitIndex + 1),
				Position))
		{
			CellChangedData[UnitIndex] = 1;
		}
	};

	RunForUnits(UnitCount, bParallel, TEXT("AlgonaSimulation_ComputeSeparation"), ComputePush);
	RunForUnits(UnitCount, bParallel, TEXT("AlgonaSimulation_ApplySeparation"), ApplyPush);
}

void UAlgonaSimulationSubsystem::UpdateSquadSummary()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSquadSummary);

	// Сводка по Squad после движения Unit:
	// - теснота (средняя по Unit, усиленная: обычно заторможены только
	//   передние ряды) — L1 в следующем тике плавно снижает скорость Squad;
	// - фактические границы — прямоугольник, реально занятый Unit, по нему
	//   в следующем тике отбираются участники L3.
	const float* CrowdingData = UnitState.Crowding.GetData();
	const float* RadiusData = UnitState.Radii.GetData();
	const FVector* PositionData = UnitState.Positions.GetData();

	for (FAlgonaSquad& Squad : Squads)
	{
		const int32 MemberCount = Squad.ActiveUnitIds.Num();
		if (MemberCount == 0)
		{
			Squad.Congestion = 0.0f;
			Squad.UnitBoundsMin = FVector2f(
				static_cast<float>(Squad.CenterLocation.X),
				static_cast<float>(Squad.CenterLocation.Y));
			Squad.UnitBoundsMax = Squad.UnitBoundsMin;
			continue;
		}

		float CrowdingSum = 0.0f;
		float MaxRadius = 0.0f;
		FVector2f BoundsMin(TNumericLimits<float>::Max());
		FVector2f BoundsMax(TNumericLimits<float>::Lowest());

		for (const uint32 UnitId : Squad.ActiveUnitIds)
		{
			const int32 UnitIndex = static_cast<int32>(UnitId) - 1;
			CrowdingSum += CrowdingData[UnitIndex];

			MaxRadius = FMath::Max(MaxRadius, RadiusData[UnitIndex]);

			const FVector2f Position(
				static_cast<float>(PositionData[UnitIndex].X),
				static_cast<float>(PositionData[UnitIndex].Y));
			BoundsMin = FVector2f::Min(BoundsMin, Position);
			BoundsMax = FVector2f::Max(BoundsMax, Position);
		}

		Squad.Congestion = FMath::Min(
			CrowdingSum / static_cast<float>(MemberCount) * SquadCongestionGain,
			1.0f);

		Squad.MaxUnitRadius = FMath::Max(MaxRadius, UE_SMALL_NUMBER);

		// Запас на радиус Unit: границы должны накрывать тела, а не центры.
		const FVector2f RadiusMargin(MaxRadius, MaxRadius);
		Squad.UnitBoundsMin = BoundsMin - RadiusMargin;
		Squad.UnitBoundsMax = BoundsMax + RadiusMargin;
	}
}

int32 UAlgonaSimulationSubsystem::UpdateUnitGrid()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateUnitGrid);

	const FAlgonaUnitStateArrays& State = UnitState;
	int32 ChangedEntities = 0;

	// Последовательно: запись в TMap сетки из нескольких потоков небезопасна.
	for (int32 UnitIndex = 0;
		UnitIndex < State.ChangedFlags.Num();
		++UnitIndex)
	{
		ChangedEntities += State.ChangedFlags[UnitIndex];

		if (State.CellChangedFlags[UnitIndex] != 0)
		{
			UnitSpatialGrid.UpdateUnit(
				static_cast<uint32>(UnitIndex + 1),
				State.Positions[UnitIndex]);
		}
	}

	return ChangedEntities;
}
