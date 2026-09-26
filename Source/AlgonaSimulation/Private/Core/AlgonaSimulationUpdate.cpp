#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitSteering.h"

#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	// Поворот больше этого угла выполняется через зеркальные слоты.
	// 135°, а не 90°: зеркальный разворот мгновенный и по углу выгоден уже
	// с 91°, но тогда отряд на повороте в 110° сначала показывает цели
	// спину и доворачивает в обратную сторону. С 135° зеркало остаётся
	// только для настоящих разворотов назад.
	constexpr double MirrorTurnMinAngleRadians =
		UE_DOUBLE_PI * 0.75 + 1.0e-3;

	// Направления совпадают, если угол между ними меньше, рад.
	constexpr double TurnToleranceRadians = 1.0e-4;

	// Скорость изменения множителя тесноты Squad, доля в секунду: строй
	// сбавляет ход за ~1 с, а восстанавливает его за ~2 с.
	constexpr float CongestionSlowRate = 1.0f;
	constexpr float CongestionRecoverRate = 0.5f;

	// Угол поворота от From к To на плоскости со знаком, рад, в [-pi, pi].
	double GetSignedYawDelta(const FVector& From, const FVector& To)
	{
		return FMath::FindDeltaAngleRadians(
			FMath::Atan2(From.Y, From.X),
			FMath::Atan2(To.Y, To.X));
	}

	// Время поворота определяется углом после возможного зеркального
	// разворота: угол больше 90° превращается в 180° минус угол.
	double GetEffectiveTurnAngle(const FVector& From, const FVector& To)
	{
		const double Angle = FMath::Abs(GetSignedYawDelta(From, To));
		return Angle > MirrorTurnMinAngleRadians ? UE_DOUBLE_PI - Angle : Angle;
	}
}

void UAlgonaSimulationSubsystem::RunSimulationStep(
	float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);

	const double StepStartSeconds =
		FPlatformTime::Seconds();

	++SimulationTick;

	// Стадия 1: команды. Приказы стресс-сценария замеров подаются в ту же
	// очередь непосредственно перед её разбором.
	if (bStressMoveEnabled)
	{
		SubmitStressMoveCommands();
	}

	ProcessPendingCommands();

	const double CommandsEndSeconds =
		FPlatformTime::Seconds();

	// Стадия 2: движение центров Squad.
	const bool bSquadCentersChanged =
		UpdateSquadCenters(DeltaTime);

	const double SquadsEndSeconds =
		FPlatformTime::Seconds();

	// Стадии 3-6: движение Unit (AlgonaSimulationMovement.cpp).
	// Режим читается один раз, чтобы весь шаг шёл в одном режиме.
	const bool bParallelMovement = IsParallelMovementEnabled();

	// L3: сетка соседей по позициям на начало тика — по ней Unit
	// заранее обходят друг друга.
	BuildLocalAvoidanceGrid(bParallelMovement);

	const double LocalGridEndSeconds =
		FPlatformTime::Seconds();

	// L2 + обход: намерение Unit, инерция, новая позиция и поворот.
	SteerUnits(DeltaTime, bParallelMovement);

	const double SteerEndSeconds =
		FPlatformTime::Seconds();

	// L3: контакт — расталкивание реально перекрывшихся Unit, затем
	// теснота Squad для замедления строя в следующем тике.
	SeparateUnits(DeltaTime, bParallelMovement);
	UpdateSquadCongestion();

	const double SeparationEndSeconds =
		FPlatformTime::Seconds();

	// Обновление Unit Grid для Unit, сменивших ячейку.
	const int32 ChangedEntities = UpdateUnitGrid();

	const double UnitGridEndSeconds =
		FPlatformTime::Seconds();

	if (bSquadCentersChanged || ChangedEntities > 0)
	{
		++StateRevision;
	}

	Metrics.SimulationTick = SimulationTick;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = UnitState.Positions.Num();
	Metrics.LastMovedEntities = ChangedEntities;
	Metrics.LastCommandsMilliseconds =
		(CommandsEndSeconds - StepStartSeconds) * 1000.0;
	Metrics.LastSquadsMilliseconds =
		(SquadsEndSeconds - CommandsEndSeconds) * 1000.0;
	Metrics.LastLocalGridMilliseconds =
		(LocalGridEndSeconds - SquadsEndSeconds) * 1000.0;
	Metrics.LastSteerMilliseconds =
		(SteerEndSeconds - LocalGridEndSeconds) * 1000.0;
	Metrics.LastSeparationMilliseconds =
		(SeparationEndSeconds - SteerEndSeconds) * 1000.0;
	Metrics.LastUnitGridMilliseconds =
		(UnitGridEndSeconds - SeparationEndSeconds) * 1000.0;
	Metrics.bLastParallelMovement = bParallelMovement;
	Metrics.LastStepMilliseconds =
		(FPlatformTime::Seconds() - StepStartSeconds) * 1000.0;

	AccumulateMetricsReportStep();
}

void UAlgonaSimulationSubsystem::ProcessPendingCommands()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ProcessCommands);

	// Команды применяются строго в порядке поступления.
	for (const FAlgonaSquadCommand& Command : PendingCommands)
	{
		if (Command.Type == EAlgonaSquadCommandType::MoveGroup)
		{
			ApplyMoveGroupCommand(Command);
			continue;
		}

		if (!Squads.IsValidIndex(Command.SquadId)
			|| Squads[Command.SquadId].SquadId != Command.SquadId)
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Command.SquadId];

		switch (Command.Type)
		{
		case EAlgonaSquadCommandType::Move:
			ApplyMoveCommand(
				Squad,
				Command.TargetLocation,
				Command.FinalDirection,
				Command.RowLength);
			break;

		case EAlgonaSquadCommandType::SetRowLength:
			// Меняются только позиции слотов, назначение Unit по слотам то же,
			// поэтому состояние Unit обновлять не нужно.
			Squad.ApplyRowLengthOrder(Command.RowLength);
			break;

		case EAlgonaSquadCommandType::MoveGroup:
			break;
		}
	}

	PendingCommands.Reset();
}

void UAlgonaSimulationSubsystem::ApplyMoveCommand(
	FAlgonaSquad& Squad,
	const FVector& TargetLocation,
	const FVector& FinalDirection,
	int32 RowLength)
{
	// Новый приказ движения полностью заменяет текущий, включая конечное
	// направление и отложенную длину строки.
	Squad.TargetCenterLocation = TargetLocation;
	Squad.TargetCenterLocation.Z = Squad.CenterLocation.Z;
	Squad.bHasMoveTarget = true;

	// Без явного направления — направление последнего отрезка пути.
	// Пока путь прямой, это вектор от центра к цели; если цель совпадает
	// с центром, направление не меняется.
	FVector Direction = FinalDirection.GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		Direction = (Squad.TargetCenterLocation - Squad.CenterLocation).GetSafeNormal2D();
	}
	Squad.FinalFacingDirection = Direction.IsNearlyZero()
		? Squad.GetForwardDirection2D()
		: Direction;

	Squad.PendingRowLength = FMath::Max(RowLength, 0);
	Squad.bHasFacingTarget = true;
	Squad.bFinalTurnStarted = false;

	// Режим движения решается один раз при получении приказа.
	const double PathLength =
		FVector::Dist2D(Squad.CenterLocation, Squad.TargetCenterLocation);

	const double MarchMinDistance = FMath::Max(
		static_cast<double>(GetFaceMovementMaxDistance()),
		FAlgonaSquad::MarchMinRadiusFactor
			* static_cast<double>(Squad.FormationLayout.Radius));

	if (PathLength > MarchMinDistance)
	{
		Squad.MoveMode = EAlgonaSquadMoveMode::March;
	}
	else if (PathLength > static_cast<double>(GetSidestepMaxDistance()))
	{
		Squad.MoveMode = EAlgonaSquadMoveMode::FaceMovement;
	}
	else
	{
		Squad.MoveMode = EAlgonaSquadMoveMode::Sidestep;
	}
}

void UAlgonaSimulationSubsystem::ApplyMoveGroupCommand(
	const FAlgonaSquadCommand& Command)
{
	// Действительные Squad группы без повторов.
	TArray<FAlgonaSquad*> GroupSquads;
	GroupSquads.Reserve(Command.SquadIds.Num());

	for (const int32 SquadId : Command.SquadIds)
	{
		if (Squads.IsValidIndex(SquadId)
			&& Squads[SquadId].SquadId == SquadId)
		{
			GroupSquads.AddUnique(&Squads[SquadId]);
		}
	}

	if (GroupSquads.IsEmpty())
	{
		return;
	}

	const FVector GroupTarget = Command.TargetLocation;

	if (GroupSquads.Num() == 1)
	{
		ApplyMoveCommand(*GroupSquads[0], GroupTarget, FVector::ZeroVector, 0);
		return;
	}

	// Направление группы одно для всех: от центра группы (среднего центров
	// Squad) к цели. Временное правило до путей по миру.
	FVector GroupCenter = FVector::ZeroVector;

	for (const FAlgonaSquad* Squad : GroupSquads)
	{
		GroupCenter += Squad->CenterLocation;
	}
	GroupCenter /= static_cast<double>(GroupSquads.Num());

	FVector GroupForward = (GroupTarget - GroupCenter).GetSafeNormal2D();
	if (GroupForward.IsNearlyZero())
	{
		GroupForward = GroupSquads[0]->GetForwardDirection2D();
	}
	const FVector GroupRight =
		FVector::CrossProduct(FVector::UpVector, GroupForward).GetSafeNormal();

	// Расстояние между точками = размер самого крупного Squad (наибольшее
	// из ширины и глубины раскладки слотов) * 1.1. Небольшое пересечение
	// допустимо.
	double MaxSquadExtent = 0.0;

	for (const FAlgonaSquad* Squad : GroupSquads)
	{
		double Extent = 0.0;

		if (!Squad->FormationLayout.Slots.IsEmpty())
		{
			FVector2f Min = Squad->FormationLayout.Slots[0].LocalOffset;
			FVector2f Max = Min;

			for (const FAlgonaFormationSlot& Slot : Squad->FormationLayout.Slots)
			{
				Min = FVector2f::Min(Min, Slot.LocalOffset);
				Max = FVector2f::Max(Max, Slot.LocalOffset);
			}

			Extent = FMath::Max(Max.X - Min.X, Max.Y - Min.Y);
		}

		// Squad из одного Unit не должен давать нулевой интервал.
		MaxSquadExtent = FMath::Max(
			MaxSquadExtent,
			FMath::Max(Extent, static_cast<double>(Squad->FormationParams.SlotSpacing)));
	}

	// Точки центров строит тот же генератор формы: длина строки —
	// округлённый вверх корень из N, неполная строка по центру.
	FAlgonaFormationParams PointParams;
	PointParams.RowLength = FMath::CeilToInt32(
		FMath::Sqrt(static_cast<double>(GroupSquads.Num())));
	PointParams.SlotSpacing = static_cast<float>(
		MaxSquadExtent * AlgonaSimulationDefaults::GroupSpacingFactor);
	PointParams.RowSpacing = PointParams.SlotSpacing;

	FAlgonaFormationLayout PointLayout;
	BuildAlgonaFormationLayout(PointParams, GroupSquads.Num(), PointLayout);

	// Назначение по текущему положению, чтобы пути не перекрещивались:
	// Squad упорядочиваются спереди назад и режутся на строки, внутри
	// строки — слева направо. Точки упорядочиваются так же и
	// сопоставляются по порядку.
	GroupSquads.Sort([&GroupTarget, &GroupForward](const FAlgonaSquad& A, const FAlgonaSquad& B)
	{
		return FVector::DotProduct(A.CenterLocation - GroupTarget, GroupForward)
			> FVector::DotProduct(B.CenterLocation - GroupTarget, GroupForward);
	});

	const int32 RowLength = PointLayout.Slots.IsEmpty() ? 1 : FMath::Max(PointParams.RowLength, 1);

	for (int32 RowStart = 0; RowStart < GroupSquads.Num(); RowStart += RowLength)
	{
		const int32 RowCount = FMath::Min(RowLength, GroupSquads.Num() - RowStart);
		TArrayView<FAlgonaSquad*>(GroupSquads.GetData() + RowStart, RowCount)
			.Sort([&GroupRight](const FAlgonaSquad& A, const FAlgonaSquad& B)
			{
				return FVector::DotProduct(A.CenterLocation, GroupRight)
					< FVector::DotProduct(B.CenterLocation, GroupRight);
			});
	}

	TArray<int32> PointOrder;
	PointOrder.Reserve(PointLayout.Slots.Num());
	for (int32 SlotIndex = 0; SlotIndex < PointLayout.Slots.Num(); ++SlotIndex)
	{
		PointOrder.Add(SlotIndex);
	}

	PointOrder.Sort([&PointLayout](int32 A, int32 B)
	{
		const FAlgonaFormationSlot& SlotA = PointLayout.Slots[A];
		const FAlgonaFormationSlot& SlotB = PointLayout.Slots[B];
		return SlotA.RowIndex != SlotB.RowIndex
			? SlotA.RowIndex < SlotB.RowIndex
			: SlotA.LocalOffset.Y < SlotB.LocalOffset.Y;
	});

	const int32 AssignedCount = FMath::Min(GroupSquads.Num(), PointOrder.Num());

	for (int32 Index = 0; Index < AssignedCount; ++Index)
	{
		const FVector PointLocation = GetAlgonaSlotWorldLocation(
			GroupTarget,
			GroupForward,
			PointLayout.Slots[PointOrder[Index]].LocalOffset);

		ApplyMoveCommand(*GroupSquads[Index], PointLocation, GroupForward, 0);
	}
}

bool UAlgonaSimulationSubsystem::ApplyMirrorTurn(FAlgonaSquad& Squad)
{
	const int32 SlotCount = Squad.ActiveUnitIds.Num();
	if (SlotCount != Squad.FormationLayout.Slots.Num() || SlotCount == 0)
	{
		return false;
	}

	// Разворот на 180°: отражается сама раскладка, а не назначение Unit.
	// Положения слотов в мире не меняются, поэтому Unit остаются на местах
	// и только разворачиваются. Меняются лишь номера слотов, и неполная
	// строка оказывается передней — до следующего Reform.
	TArray<int32> NewSlotForOldSlot;
	MirrorAlgonaFormationLayout(Squad.FormationLayout, NewSlotForOldSlot);

	TArray<uint32> ReorderedUnitIds;
	ReorderedUnitIds.SetNumUninitialized(SlotCount);

	for (int32 OldSlotIndex = 0; OldSlotIndex < SlotCount; ++OldSlotIndex)
	{
		const int32 NewSlotIndex = NewSlotForOldSlot[OldSlotIndex];
		const uint32 UnitId = Squad.ActiveUnitIds[OldSlotIndex];

		ReorderedUnitIds[NewSlotIndex] = UnitId;
		UnitState.SlotIndices[static_cast<int32>(UnitId) - 1] = NewSlotIndex;
	}

	Squad.ActiveUnitIds = MoveTemp(ReorderedUnitIds);
	Squad.FacingDirection = -Squad.GetForwardDirection2D();
	++Squad.FormationRevision;
	return true;
}

bool UAlgonaSimulationSubsystem::UpdateSquadCenters(
	float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSquads);

	bool bAnySquadChanged = false;

	// Отладочная подмена скорости всех Squad: удобно подбирать скорость,
	// не пересобирая проект.
	const float SquadMoveSpeedOverride = GetSquadMoveSpeedOverride();
	const float CongestionSlowdown = FMath::Clamp(GetSquadCongestionSlowdown(), 0.0f, 1.0f);

	for (FAlgonaSquad& Squad : Squads)
	{
		if (SquadMoveSpeedOverride > 0.0f)
		{
			Squad.CenterMoveSpeed = SquadMoveSpeedOverride;
		}

		if ((!Squad.bHasMoveTarget && !Squad.bHasFacingTarget)
			|| DeltaTime <= 0.0f)
		{
			Squad.CenterVelocity = FVector::ZeroVector;
			Squad.CenterSpeed = 0.0f;
			Squad.YawRate = 0.0f;
			continue;
		}

		// Плавный старт после зеркального разворота: пока Unit разворачиваются
		// на месте, скорость Squad растёт от нуля до полной. Отдельная
		// инерция движения и поворота появится на шаге 10.
		double StartSpeedScale = 1.0;

		if (Squad.MirrorTurnStartRemainingSeconds > 0.0f)
		{
			Squad.MirrorTurnStartRemainingSeconds =
				FMath::Max(Squad.MirrorTurnStartRemainingSeconds - DeltaTime, 0.0f);

			StartSpeedScale = 1.0 - static_cast<double>(
				Squad.MirrorTurnStartRemainingSeconds
					/ FAlgonaSquad::MirrorTurnStartSeconds);
		}

		const FVector OldCenterLocation = Squad.CenterLocation;

		FVector ToTarget = Squad.bHasMoveTarget
			? Squad.TargetCenterLocation - Squad.CenterLocation
			: FVector::ZeroVector;
		ToTarget.Z = 0.0;

		const double DistanceToTarget = ToTarget.Length();

		// Составной приказ: ширина строя применяется за 10 м до цели
		// (или сразу, если путь короче). Reform сохраняет центр.
		if (Squad.bHasMoveTarget
			&& Squad.PendingRowLength > 0
			&& DistanceToTarget <= FAlgonaSquad::RowLengthApplyDistanceCm)
		{
			Squad.ApplyRowLengthOrder(Squad.PendingRowLength);
			Squad.PendingRowLength = 0;
		}

		const bool bNeedsMove =
			Squad.bHasMoveTarget && DistanceToTarget > KINDA_SMALL_NUMBER;
		const FVector MoveDirection = bNeedsMove
			? ToTarget / DistanceToTarget
			: FVector::ZeroVector;

		// Скорость крайнего слота = скорость центра + скорость от вращения.
		// Пока Squad идёт, вращению остаётся только часть общего бюджета.
		const double SlotSpeedBudget = FMath::Max(
			static_cast<double>(Squad.MaxSlotSpeedFactor * Squad.CenterMoveSpeed
				- Squad.CenterSpeed),
			static_cast<double>(Squad.MinTurnRateFactor * Squad.CenterMoveSpeed));

		const double MaxYawRate = FMath::Min(
			static_cast<double>(Squad.GetMaxYawRate()),
			static_cast<double>(Squad.GetMaxYawRate())
				* SlotSpeedBudget
				/ static_cast<double>(Squad.TurnSpeedFactor * Squad.CenterMoveSpeed));

		// 1. Желаемое направление. На марше — по ходу движения, пока до цели
		// дальше, чем Squad пройдёт за время поворота к конечному направлению
		// (путь = скорость * угол / угловая скорость). Так поворот
		// заканчивается ровно по прибытии.
		FVector DesiredForward = Squad.FinalFacingDirection;

		if (bNeedsMove
			&& Squad.MoveMode == EAlgonaSquadMoveMode::March
			&& !Squad.bFinalTurnStarted)
		{
			const double FinalTurnDistance =
				static_cast<double>(Squad.CenterMoveSpeed)
				* GetEffectiveTurnAngle(MoveDirection, Squad.FinalFacingDirection)
				/ MaxYawRate
				* FAlgonaSquad::FinalTurnDistanceFactor;

			if (DistanceToTarget <= FinalTurnDistance)
			{
				Squad.bFinalTurnStarted = true;
			}
			else
			{
				DesiredForward = MoveDirection;
			}
		}

		const bool bDesiredIsFinal = DesiredForward.Equals(Squad.FinalFacingDirection);

		// 2. Разворот больше 90° — мгновенно через зеркальные слоты,
		// остаток — обычным поворотом.
		double DeltaYaw = GetSignedYawDelta(Squad.GetForwardDirection2D(), DesiredForward);
		bool bSquadChanged = false;

		if (FMath::Abs(DeltaYaw) > MirrorTurnMinAngleRadians
			&& ApplyMirrorTurn(Squad))
		{
			// Остаток поворота и движение начинаются плавно с нулевой скорости.
			Squad.MirrorTurnStartRemainingSeconds = FAlgonaSquad::MirrorTurnStartSeconds;
			StartSpeedScale = 0.0;

			DeltaYaw = GetSignedYawDelta(Squad.GetForwardDirection2D(), DesiredForward);
			bSquadChanged = true;
		}

		const bool bNeedsTurn = FMath::Abs(DeltaYaw) > TurnToleranceRadians;

		// 3. Поворот строя вокруг центра. Движение и поворот идут
		// одновременно, каждый со своим разгоном и торможением.
		// Желаемая угловая скорость ограничена и максимумом, и торможением
		// к нужному углу, чтобы строй встал точно в направлении, а не качался.
		const double YawAcceleration = static_cast<double>(Squad.GetYawAcceleration());
		const double YawDeceleration = YawAcceleration * FAlgonaSquad::DecelerationFactor;

		double DesiredYawRate = FMath::Min(
			MaxYawRate * StartSpeedScale,
			static_cast<double>(AlgonaUnitSteering::GetArrivalSpeedLimit(
				static_cast<float>(FMath::Abs(DeltaYaw)),
				static_cast<float>(YawDeceleration))));

		DesiredYawRate = bNeedsTurn
			? FMath::Sign(DeltaYaw) * DesiredYawRate
			: 0.0;

		Squad.YawRate = AlgonaUnitSteering::StepValueTowards(
			Squad.YawRate,
			static_cast<float>(DesiredYawRate),
			static_cast<float>(YawAcceleration),
			static_cast<float>(YawDeceleration),
			DeltaTime);

		if (Squad.YawRate != 0.0f)
		{
			// Поворот не перелетает нужный угол.
			const double YawStep = FMath::Clamp(
				static_cast<double>(Squad.YawRate) * DeltaTime,
				FMath::Min(DeltaYaw, 0.0),
				FMath::Max(DeltaYaw, 0.0));

			const FVector Forward = Squad.GetForwardDirection2D();
			const double NewYaw = FMath::Atan2(Forward.Y, Forward.X) + YawStep;

			Squad.FacingDirection = FVector(FMath::Cos(NewYaw), FMath::Sin(NewYaw), 0.0);
			Squad.YawRate = static_cast<float>(YawStep / DeltaTime);
			DeltaYaw -= YawStep;
			bSquadChanged = true;
		}

		// Поворот к конечному направлению закончен: убираем накопленную
		// погрешность угла.
		if (bDesiredIsFinal && FMath::Abs(DeltaYaw) <= TurnToleranceRadians)
		{
			Squad.FacingDirection = Squad.FinalFacingDirection;

			if (!Squad.bHasMoveTarget || !bNeedsMove)
			{
				Squad.bHasFacingTarget = false;
			}
		}

		// Теснота: если Unit Squad тормозят и обходят соседей, строй сбавляет
		// ход и не вдавливает их в толпу. Множитель меняется плавно:
		// замедляется быстрее, чем восстанавливается.
		Squad.CongestionSpeedScale = AlgonaUnitSteering::StepValueTowards(
			Squad.CongestionSpeedScale,
			1.0f - Squad.Congestion * CongestionSlowdown,
			CongestionRecoverRate,
			CongestionSlowRate,
			DeltaTime);

		// 4. Движение центра по прямой к цели с разгоном и торможением.
		// Желаемая скорость ограничена торможением к цели, поэтому центр
		// встаёт ровно в цели, а не проскакивает её.
		const double MoveAcceleration = static_cast<double>(Squad.GetMoveAcceleration());
		const double MoveDeceleration = static_cast<double>(Squad.GetMoveDeceleration());

		const double DesiredSpeed = bNeedsMove
			? FMath::Min3(
				static_cast<double>(Squad.CenterMoveSpeed * Squad.CongestionSpeedScale),
				static_cast<double>(Squad.CenterMoveSpeed) * StartSpeedScale,
				static_cast<double>(AlgonaUnitSteering::GetArrivalSpeedLimit(
					static_cast<float>(DistanceToTarget),
					static_cast<float>(MoveDeceleration))))
			: 0.0;

		Squad.CenterSpeed = AlgonaUnitSteering::StepValueTowards(
			Squad.CenterSpeed,
			static_cast<float>(DesiredSpeed),
			static_cast<float>(MoveAcceleration),
			static_cast<float>(MoveDeceleration),
			DeltaTime);

		if (Squad.bHasMoveTarget)
		{
			const double MoveDistance =
				static_cast<double>(Squad.CenterSpeed) * DeltaTime;

			if (DistanceToTarget <= MoveDistance + KINDA_SMALL_NUMBER)
			{
				Squad.CenterLocation = Squad.TargetCenterLocation;
				Squad.CenterSpeed = 0.0f;
				Squad.bHasMoveTarget = false;
			}
			else
			{
				Squad.CenterLocation += MoveDirection * MoveDistance;
			}
		}

		// Скорость центра за этот тик — упреждение для L2: Unit сразу идут
		// вместе с Squad, а не догоняют свои слоты.
		Squad.CenterVelocity =
			(Squad.CenterLocation - OldCenterLocation) / static_cast<double>(DeltaTime);
		Squad.CenterVelocity.Z = 0.0;

		bSquadChanged |= !OldCenterLocation.Equals(Squad.CenterLocation, KINDA_SMALL_NUMBER);
		bAnySquadChanged |= bSquadChanged;

		if (IsSquadSpatialGridEnabled())
		{
			SquadSpatialGrid.UpdateSquad(
				Squad.SquadId,
				OldCenterLocation,
				Squad.CenterLocation);
		}
	}

	return bAnySquadChanged;
}
